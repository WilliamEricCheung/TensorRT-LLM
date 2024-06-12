/*
 * Copyright (c) 2020-2023, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CUDART_VERSION
#error CUDART_VERSION Undefined!
#elif (CUDART_VERSION >= 11050)
#include <cub/cub.cuh>
#else
#include "3rdparty/cub/cub.cuh"
#endif

#include "tensorrt_llm/common/assert.h"
#include "tensorrt_llm/common/cudaUtils.h"
#include "tensorrt_llm/common/reduceKernelUtils.cuh"
#include "tensorrt_llm/common/stringUtils.h"
#include "tensorrt_llm/kernels/beamSearchKernels.h"
#include "tensorrt_llm/kernels/decodingCommon.h"

using namespace tensorrt_llm::common;

namespace tensorrt_llm
{
namespace kernels
{

#define DO_SPLIT_SMALL_TOP_K_SOFTMAX

#define TOPK_FP16_STORAGE 0

#pragma nv_diag_suppress static_var_with_dynamic_init

template <typename T, int PAD_2K, int THREADBLOCK_SIZE>
__launch_bounds__(THREADBLOCK_SIZE) __global__
    void beamStage3Kernel(int const* __restrict pTempId, T const* __restrict pTempVal, BeamHypotheses bh)
{
    int const bid = blockIdx.x;          // Index of Batch
    int const tid = threadIdx.x;
    int const nMBS{bh.nMaxBatchSize};    // Only for bh.logProbsTiled
    int const nBS{bh.nBatchSize};
    int const gbid{nBS * bh.nIte + bid}; // Global batch index, TODO: distinguish bid and gbid in the future
    int const nBM{bh.nBeamWidth};
    int const nCandidate{nBM * nBM * 2}; // Keep top 2K candidates from each beam output
    int const nV{bh.nVocabSize};
    float const diversityRate{bh.diversityRates[gbid]};
    float const lengthPenalty{bh.lengthPenalties[gbid]};
    int const earlyStopping{bh.earlyStoppings[gbid]};
    T const MAX_T_VAL = std::is_same_v<T, half> ? HALF_FLT_MAX : FLT_MAX;

    __shared__ int nBeamForNextStep; // Only used by thread of tid == 0
    __shared__ float smemCumLogProbs[PAD_2K / 2];

    if (tid == 0)
    {
        nBeamForNextStep = 0;
    }
    if (tid < nBM)
    {
        smemCumLogProbs[tid] = bh.cumLogProbs[bid * nBM + tid];
    }
    __syncthreads();

    if (bh.numBeamsCBA != nullptr)
    {
        // Beam search is enabled
        if (bh.numBeamsCBA[gbid] == 0 && tid == 0)
        {
            // Initialize worst score in the first call
            bh.minNormedScoresCBA[gbid] = FLT_MAX;
        }
        else if (earlyStopping == 1 && bh.numBeamsCBA[gbid] == nBM
            || earlyStopping != 1 && bh.finished[bid * nBM].isFinished())
        {
            // Condition of early return:
            // 1. In EarlyStopping mode, and we have got enough beams
            // 2. In NonEarlyStopping mode, and this batch has been marked as done
            // TODO: improve the condition like below
            // earlyStopping == 1 && bh.numBeamsCBA[gbid] == nBM || earlyStopping != 1 && bh.batchDones[bid]
            return;
        }
    }

    // Get top 2K tokens from candidates
    pTempId += bid * nCandidate;
    pTempVal += bid * nCandidate;

    using KVPair = cub::KeyValuePair<int, T>;
    KVPair topKVPairPartial{nCandidate - 1, -MAX_T_VAL};
    cub::ArgMax argmax;
    extern __shared__ char smem[];
    T* smemVal = reinterpret_cast<T*>(smem);

    for (int i = tid; i < nCandidate; i += THREADBLOCK_SIZE)
    {
        int const index = bh.numBeamsCBA == nullptr ? i % nBM : i / 2 / nBM;
        T const val = pTempVal[i] + static_cast<T>(diversityRate * index);
        topKVPairPartial = argmax(topKVPairPartial, {i, val});
        smemVal[i] = val;
    }
    __syncthreads();

    using BlockReduce = cub::BlockReduce<KVPair, THREADBLOCK_SIZE>;
    __shared__ typename BlockReduce::TempStorage smemReduceBuffer;
    __shared__ KVPair smemTopKV[PAD_2K];
    __shared__ int threadToUpdate;

    for (int i = 0; i < 2 * nBM; ++i)
    {
        KVPair topKVPair = BlockReduce(smemReduceBuffer).Reduce(topKVPairPartial, argmax);
        if (tid == 0)
        {
            smemTopKV[i] = topKVPair;
            smemVal[topKVPair.key] = -MAX_T_VAL;
            threadToUpdate = topKVPair.key % THREADBLOCK_SIZE;
        }
        __syncthreads();
        // Only one thread needs to update the old partial before the next block reduce.
        // No need to do this in the last iteration.
        if (tid == threadToUpdate && i < 2 * nBM - 1)
        {
            topKVPairPartial.key = nCandidate - 1;
            topKVPairPartial.value = -MAX_T_VAL;
            for (int index = tid; index < nCandidate; index += THREADBLOCK_SIZE)
            {
                topKVPairPartial = argmax(topKVPairPartial, {index, smemVal[index]});
            }
        }
    }

    if (tid == 0)
    {
        // Select finished beams into CBA or select tokens for next step sequentially
        // Reference (might be changed along HF in the future):
        // https://github.com/huggingface/transformers/blob/main/src/transformers/generation/beam_search.py#L272
        for (int i = 0; i < 2 * nBM; ++i)
        {
            int const topKey = smemTopKV[i].key;
            T const topValue = smemTopKV[i].value;
            bool const isEndToken = pTempId[topKey] % nV == bh.endIds[bid];
            if (i < nBM && bh.numBeamsCBA != nullptr && isEndToken)
            {
                // Condition of this branch
                // This token is end-token and belongs to top nBM range in Beam search mode
                int const nSeqLen = bh.sequenceLengths[bid * nBM + i] + 1 - bh.inputLengths[gbid * nBM + i];
                float const score = applyLengthPenalty(topValue, nSeqLen, lengthPenalty);
                int nCBA = bh.numBeamsCBA[gbid];
                if (nCBA == nBM)
                {
                    // There are already nBM beams
                    if (score < bh.minNormedScoresCBA[gbid])
                    {
                        // Current score is worse than the worst one in candidate beams
                        if (earlyStopping)
                        {
                            // Stop since we have got enough beams
                            break;
                        }
                        else
                        {
                            // Continue since there might be longer but better beams
                            continue;
                        }
                    }
                    else
                    {
                        // Current score is better than the worst one in candidate beams
                        // Find the candidate beam index with the worst score and erase it
                        for (int j = 0; j < nBM; j++)
                        {
                            if (bh.normedScoresCBA[gbid * (nBM * 2) + j] == bh.minNormedScoresCBA[gbid])
                            {
                                nCBA = j;
                                bh.numBeamsCBA[gbid]--;
                                bh.minNormedScoresCBA[gbid] = FLT_MAX;
                                bh.normedScoresCBA[gbid * (nBM * 2) + j] = score;
                                for (int l = 0; l < nBM; l++)
                                {
                                    bh.minNormedScoresCBA[gbid]
                                        = min(bh.minNormedScoresCBA[gbid], bh.normedScoresCBA[gbid * (nBM * 2) + l]);
                                }
                                break;
                            }
                        }
                    }
                }
                // Copy finished beam from work tree to CBA
                // The last token
                int indexPrev = (pTempId[topKey] / nV) % nBM;
                int const step = bh.sequenceLengths[bid * nBM + indexPrev];
                int const offsetCBA = (gbid * nBM * 2 + nCBA) * bh.nMaxSeqLen;
                bh.outputIdsCBA[offsetCBA + step] = bh.endIds[bid];
                if (bh.logProbsCBA != nullptr)
                {
                    bh.logProbsCBA[offsetCBA + step]
                        = (float) pTempVal[topKey] - smemCumLogProbs[(pTempId[topKey] / nV) % nBM];
                }
                // Previous tokens
                for (int j = step - 1; j >= 0; j--)
                {
                    bh.outputIdsCBA[offsetCBA + j] = bh.outputIdsPtr[bid][indexPrev * bh.nMaxSeqLen + j];
                    indexPrev = bh.parentIdsPtr[bid][indexPrev * bh.nMaxSeqLen + j];
                }
                if (bh.logProbsCBA != nullptr && bh.logProbsTiled != nullptr)
                {
                    indexPrev = (pTempId[topKey] / nV) % nBM;
                    for (int j = step - 1; j >= 0; j--)
                    {
                        int const index = (j * nMBS + gbid) * nBM + indexPrev;
                        bh.logProbsCBA[offsetCBA + j] = bh.logProbsTiled[index];
                        indexPrev = bh.parentIdsPtr[bid][indexPrev * bh.nMaxSeqLen + j];
                    }
                }
                // Other parameters
                int const index = gbid * (nBM * 2) + nCBA;
                bh.sequenceLengthsCBA[index] = step;
                bh.normedScoresCBA[index] = score;
                bh.minNormedScoresCBA[gbid] = min(bh.minNormedScoresCBA[gbid], bh.normedScoresCBA[index]);
                bh.numBeamsCBA[gbid]++;
                bh.cumLogProbsCBA[index] = (float) pTempVal[topKey];
            }
            else if (i < nBM || bh.numBeamsCBA != nullptr && !isEndToken)
            {
                // Condition of this branch
                // 1. bh.numBeamsCBA == nullptr && i <  nBM, i.e., beam search is disable
                // 2. bh.numBeamsCBA != nullptr && i <  nBM && isEndToken == false, i.e., add token at the end
                // 3. bh.numBeamsCBA != nullptr && i >= nBM && isEndToken == false, i.e., add token at the end
                int const step = bh.sequenceLengths[bid * nBM + nBeamForNextStep];
                // Copy the selected token to work tree
                bh.outputIdsPtr[bid][nBeamForNextStep * bh.nMaxSeqLen + step] = pTempId[topKey];
                if (bh.logProbsTiled != nullptr)
                {
                    int const index = step * nMBS * nBM + bid * nBM + nBeamForNextStep;
                    int const indexBeam = pTempId[topKey] / nV % nBM;
                    bh.logProbsTiled[index] = (float) pTempVal[topKey] - smemCumLogProbs[indexBeam];
                }
                bh.cumLogProbs[bid * nBM + nBeamForNextStep] = (float) pTempVal[topKey];
                nBeamForNextStep++;
            }
            else
            {
                // Condition of this branch, which we do nothing for it
                // 1. bh.numBeamsCBA == nullptr && i >= nBM, i.e., beam search is disable
                // 2. bh.numBeamsCBA != nullptr && i >= nBM && isEndToken == true, i.e., ignore the worse beams
            }

            if (nBeamForNextStep >= nBM)
            {
                // Condition of this branch
                // 1. In EarlyStopping mode, and get enough candidate beams
                // 2. In EarlyStopping mode, and get enough tokens for the next generation step
                // 3. In NonEarlyStopping mode, and get enough tokens for the next generation step
                // TODO: improve the condition like below
                // earlyStopping == 1 && bh.numBeamsCBA[gbid] >= nBM || nBeamForNextStep >= nBM
                break;
            }
        }
    }

    // Update bh.batchDones
    if (tid == 0 && bh.numBeamsCBA != nullptr)
    {
        if (bh.numBeamsCBA[bid] < nBM)
        {
            // no enough beams
            bh.batchDones[bid] = false;
        }
        else if (earlyStopping == 1)
        {
            // enough candidate beams in EarlyStopping mode
            bh.batchDones[bid] = true;
        }
        else
        {
            // enough beams in NonEarlyStopping mode
            int nSeqLen = bh.sequenceLengths[bid * nBM] + 1 - bh.inputLengths[gbid * nBM];
            float const bestCumLogProbs = smemTopKV[0].value;
            // According to semantics of HF, smemTopKV[0].value is used as bestCumLogProbs
            // But maybe bh.cumLogProbs[bid * nBM + i] is more suitable?
            // https://github.com/huggingface/transformers/blob/main/src/transformers/generation/beam_search.py#L307
            if (earlyStopping != 0 && lengthPenalty > 0.0f)
            {
                // Specialization for earlyStopping == "never" and lengthPenalty > 0 in HF
                nSeqLen = bh.nMaxSeqLen - bh.inputLengths[gbid * nBM];
            }
            float const bestAttainableScore = applyLengthPenalty(bestCumLogProbs, nSeqLen, lengthPenalty);
            bh.batchDones[bid] = bh.minNormedScoresCBA[gbid] >= bestAttainableScore;
        }
    }
    __syncthreads();

    // Update sequenceLengths, parentIdsPtr, outputIdsPtr and finished
    __shared__ int smemSeqLen[PAD_2K / 2];
    if (tid < nBM)
    {
        smemSeqLen[tid] = bh.sequenceLengths[bid * nBM + tid];
    }
    __syncthreads();

    if (tid < nBM)
    {
        int const indexBatchBeam = bid * nBM + tid;
        int const step = smemSeqLen[tid];
        if (!bh.finished[indexBatchBeam].isFinished())
        {
            smemSeqLen[tid]++;
        }
        int const newId = bh.outputIdsPtr[bid][tid * bh.nMaxSeqLen + step];
        int const newBeamId = (newId / nV) % nBM;
        int const newTokenId = newId % nV;
        bh.sequenceLengths[indexBatchBeam] = smemSeqLen[newBeamId];
        if (newTokenId == bh.endIds[bid])
        {
            bh.finished[indexBatchBeam].setFinishedEOS();
        }
        bh.parentIdsPtr[bid][tid * bh.nMaxSeqLen + step] = newBeamId;
        bh.outputIdsPtr[bid][tid * bh.nMaxSeqLen + step] = newTokenId;
        if ((earlyStopping == 1) && (bh.numBeamsCBA != nullptr && bh.numBeamsCBA[gbid] == nBM)
            || (earlyStopping != 1) && bh.batchDones[bid])
        {
            bh.batchDones[bid] = true;
            bh.finished[indexBatchBeam].setFinished();
        }
    }
}

struct __align__(8) MD
{
    float m;
    float d;
};

__device__ __forceinline__ MD reduce_md_op(MD a, MD b)
{
    bool const isABigger = a.m > b.m;
    MD const bigger = isABigger ? a : b;
    MD const smaller = isABigger ? b : a;
    MD res{bigger.m, bigger.d + smaller.d * __expf(smaller.m - bigger.m)};
    return res;
}

template <typename T, int PAD_K>
struct TopKMD
{
    MD md;
    TopK<T, PAD_K> topk;
};

template <typename T, int PAD_K>
__device__ __forceinline__ TopKMD<T, PAD_K> reduce_topk_md_op(TopKMD<T, PAD_K> const& a, TopKMD<T, PAD_K> const& b)
{
    TopKMD<T, PAD_K> res;
    res.md = reduce_md_op(a.md, b.md);
    res.topk = reduce_topk_op(a.topk, b.topk);
    return res;
}

template <typename T, int ITEMS_PER_THREAD, int PAD_K, int THREADBLOCK_SIZE>
__launch_bounds__(THREADBLOCK_SIZE) __global__ void beamKernel(T const* __restrict logits, T const* __restrict bias,
    int* __restrict pTempId, T* __restrict pTempVal, BeamHypotheses bh)
{
    int const tid = threadIdx.x;
    int const bid = blockIdx.x;
    int const nBM{bh.nBeamWidth};
    int const nV{bh.nVocabSize};
    int const* endIds{bh.endIds};
    float const* cumLogProbs{bh.cumLogProbs};
    FinishedState const* finished{bh.finished};
    T const MAX_T_VAL = std::is_same_v<T, half> ? HALF_FLT_MAX : FLT_MAX;

    TopKMD<float, PAD_K> partial;
    partial.md.m = -MAX_T_VAL;
    partial.md.d = 0.0F;
    partial.topk.init();

    if (finished[bid].isFinished())
    {
        for (int i = tid; i < nV; i += THREADBLOCK_SIZE)
        {
            float const val = i == endIds[bid / nBM] ? MAX_T_VAL : -MAX_T_VAL;
            partial.md = reduce_md_op(partial.md, {val, 1.0F});
            partial.topk.insert(val, i);
        }
    }
    else
    {
        T const* local_logits = logits + bid * nV;
        for (int i = tid; i < nV; i += THREADBLOCK_SIZE)
        {
            float const val = local_logits[i] + bias[i];
            partial.md = reduce_md_op(partial.md, {val, 1.0F});
            partial.topk.insert(val, i);
        }
    }

    typedef cub::BlockReduce<TopKMD<float, PAD_K>, THREADBLOCK_SIZE> BlockReduce;
    __shared__ typename BlockReduce::TempStorage smemReduceBuffer;

    TopKMD<float, PAD_K> total = BlockReduce(smemReduceBuffer).Reduce(partial, reduce_topk_md_op<float, PAD_K>);

    if (tid == 0)
    {
        int* localTopKId = pTempId + bid * nBM;
        T const* localTopKVal = pTempVal + bid * nBM;
        float const total_m = total.md.m;
        float const total_d = logf(total.md.d);
        float localCumLogProbs = cumLogProbs[bid];
        for (int i = 0; i < nBM; ++i)
        {
            localTopKId[i] = total.topk.p[i] + bid * nV;
            localTopKVal[i] = total.topk.u[i] - total_m - total_d + localCumLogProbs;
        }
    }
}

template <typename T, int ITEMS_PER_THREAD, int PAD_2K, int THREADBLOCK_SIZE>
__launch_bounds__(THREADBLOCK_SIZE, 1) __global__
    void beamStage1BaseKernel(T const* __restrict logits, T const* __restrict bias, float* __restrict pTemp,
        int const* __restrict endIds, FinishedState const* __restrict finished, int nBM, int nV)
{
    // Compare to beamStage1FastKernel, here is no share memory for storage of logits,
    // and each ThreadBlock is responsible for `nV / nVPart` elements
    constexpr int PACKED_TOP_KMD_SIZE = 2 * PAD_2K + 2;
    int const tid = threadIdx.x;
    int const bid = blockIdx.x;
    int const nVLocal = (nV + gridDim.y - 1) / gridDim.y;
    int const section_start = nVLocal * blockIdx.y;
    int const section_end = std::min(section_start + nVLocal, nV);
    T const MAX_T_VAL = std::is_same_v<T, half> ? HALF_FLT_MAX : FLT_MAX;

    // Load element from logits to do reduce_md and argmax meanwhile
#if TOPK_FP16_STORAGE == 1
    TopKMD<__half, PAD_2K> partial;
#else
    TopKMD<T, PAD_2K> partial;
#endif
    partial.md.m = -MAX_T_VAL;
    partial.md.d = 0.0F;
    partial.topk.init();

    if (finished[bid].isFinished())
    {
#pragma unroll 1
        for (int i = section_start + tid; i < section_end; i += THREADBLOCK_SIZE)
        {
            float const val = (i == endIds[bid / nBM]) ? MAX_T_VAL : -MAX_T_VAL;
            MD const new_elem_md{val, 1.0F};
            partial.md = reduce_md_op(partial.md, new_elem_md);
            partial.topk.insert(val, i);
        }
    }
    else
    {
        T const* local_logits = logits + bid * nV;
#pragma unroll 1
        for (int i = section_start + tid; i < section_end; i += THREADBLOCK_SIZE)
        {
            T const b = bias == nullptr ? (T) 0.0f : bias[i];
            T const val = local_logits[i] + b;
            MD new_elem_md{val, 1.0F};
            partial.md = reduce_md_op(partial.md, new_elem_md);
            partial.topk.insert(val, i);
        }
    }

    // Search the top 2K elements among `nV` elements and write into smemOutput
#if TOPK_FP16_STORAGE == 1
    typedef cub::BlockReduce<TopKMD<__half, PAD_2K>, THREADBLOCK_SIZE> BlockReduce;
    __shared__ typename BlockReduce::TempStorage smemReduceBuffer;
    TopKMD<__half, PAD_2K> total = BlockReduce(smemReduceBuffer).Reduce(partial, reduce_topk_md_op<__half, PAD_2K>);
#else
    typedef cub::BlockReduce<TopKMD<T, PAD_2K>, THREADBLOCK_SIZE> BlockReduce;
    __shared__ typename BlockReduce::TempStorage smemReduceBuffer;
    TopKMD<T, PAD_2K> total = BlockReduce(smemReduceBuffer).Reduce(partial, reduce_topk_md_op<T, PAD_2K>);
#endif
    __shared__ float smemOutput[PACKED_TOP_KMD_SIZE];

    if (tid == 0)
    {
        for (int i = 0; i < 2 * nBM; i++)
        {
            int const index = bid * nV + total.topk.p[i];
            reinterpret_cast<int*>(smemOutput)[i] = index;
            smemOutput[PAD_2K + i] = total.topk.u[i];
        }
        smemOutput[2 * PAD_2K] = total.md.d;
        smemOutput[2 * PAD_2K + 1] = total.md.m;
    }
    __syncthreads();

    // Write the smemOutput into pTemp
    float* local_temp_buffer = pTemp + bid * PACKED_TOP_KMD_SIZE * gridDim.y + blockIdx.y * PACKED_TOP_KMD_SIZE;
#pragma unroll
    for (int i = tid; i < PACKED_TOP_KMD_SIZE; i += THREADBLOCK_SIZE)
    {
        local_temp_buffer[i] = smemOutput[i];
    }
}

template <typename T, int ITEMS_PER_THREAD, int PAD_2K, int THREADBLOCK_SIZE>
__launch_bounds__(THREADBLOCK_SIZE, 1) __global__ void beamStage1FastKernel(T const* __restrict logits,
    T const* __restrict bias, float* __restrict pTemp, int const* __restrict endIds,
    FinishedState const* __restrict finished, int const nBM, int const nV, int const nVLocal)
{
    constexpr int PACKED_TOP_KMD_SIZE = 2 * PAD_2K + 2;
    int const tid = threadIdx.x;
    int const bid = blockIdx.x;
    int const section_start = nVLocal * blockIdx.y;
    int const section_end = std::min(section_start + nVLocal, nV);
    int const valid_smem_length = section_end - section_start;
    T const MAX_T_VAL = std::is_same_v<T, half> ? HALF_FLT_MAX : FLT_MAX;

    // Load element from logits to smemLogProbs, doing reduce_md and argmax meanwhile
    // Each thread is responsible for `nVLocal / THREADBLOCK_SIZE` elements
    extern __shared__ char smem[];
    T* smemLogProbs = reinterpret_cast<T*>(smem);

    MD partial_md{-MAX_T_VAL, 0.0f};

#if TOPK_FP16_STORAGE == 1
    using KVPair = cub::KeyValuePair<int, __half>;
#else
    using KVPair = cub::KeyValuePair<int, T>;
#endif
    KVPair topKVPairPartial{nV - 1, -MAX_T_VAL};
    cub::ArgMax argmax;

    if (finished[bid].isFinished())
    {
#pragma unroll 1
        for (int i = section_start + tid; i < section_end; i += THREADBLOCK_SIZE)
        {
            float const val = (i == endIds[bid / nBM]) ? MAX_T_VAL : -MAX_T_VAL;
            int const smem_index = i - section_start;
            smemLogProbs[smem_index] = val;
            MD const new_elem_md{val, 1.0F};
            partial_md = reduce_md_op(partial_md, new_elem_md);
            KVPair const new_elem_topk{smem_index, val};
            topKVPairPartial = argmax(topKVPairPartial, new_elem_topk);
        }
    }
    else
    {
        T const* local_logits = logits + bid * nV;
#pragma unroll 1
        for (int i = section_start + tid; i < section_end; i += THREADBLOCK_SIZE)
        {
            T const b = bias == nullptr ? (T) 0.0f : bias[i];
            T const val = local_logits[i] + b;
            int const smem_index = i - section_start;
            smemLogProbs[smem_index] = val;
            MD new_elem_md{val, 1.0F};
            partial_md = reduce_md_op(partial_md, new_elem_md);
            KVPair new_elem_topk{smem_index, val};
            topKVPairPartial = argmax(topKVPairPartial, new_elem_topk);
        }
    }
    __syncthreads();

    // Search the top 2K elements among `nVLocal` elements of this ThreadBlock and write into smemOutput
    __shared__ float smemOutput[PACKED_TOP_KMD_SIZE];
    __shared__ int threadToUpdate;

    using BlockReduceMD = cub::BlockReduce<MD, THREADBLOCK_SIZE>;
    using BlockReduceTopK = cub::BlockReduce<KVPair, THREADBLOCK_SIZE>;

    __shared__ union
    {
        typename BlockReduceTopK::TempStorage topk;
        typename BlockReduceMD::TempStorage md;
    } smemReduceBuffer;

    for (int i = 0; i < 2 * nBM; ++i)
    {
        // Pop the element with largest value to "smemOutput" per iteration
        KVPair topKVPair = BlockReduceTopK(smemReduceBuffer.topk).Reduce(topKVPairPartial, argmax);
        if (tid == 0)
        {
            int const index = bid * nV + section_start + topKVPair.key;
            reinterpret_cast<int*>(smemOutput)[i] = index;
            smemOutput[PAD_2K + i] = topKVPair.value;
            smemLogProbs[topKVPair.key] = -MAX_T_VAL; // pollute the value of the popped element
            threadToUpdate = topKVPair.key % THREADBLOCK_SIZE;
        }
        __syncthreads();

        if (tid == threadToUpdate && i < 2 * nBM - 1)
        {
            // The thread popped the element need to update its topKVPairPartial
            // No need to do this in the last iteration
            topKVPairPartial.key = nV - 1;
            topKVPairPartial.value = -MAX_T_VAL;
            for (int index = tid; index < valid_smem_length; index += THREADBLOCK_SIZE)
            {
                topKVPairPartial = argmax(topKVPairPartial, {index, smemLogProbs[index]});
            }
        }
    }

    // Do reduce_md among the top 2K elements in the smemOutput and write into tail of smemOutput
    auto reduce_md_func = [](const MD& a, const MD& b) { return reduce_md_op(a, b); };
    MD total_md = BlockReduceMD(smemReduceBuffer.md).Reduce(partial_md, reduce_md_func);
    if (tid == 0)
    {
        smemOutput[2 * PAD_2K] = total_md.d;
        smemOutput[2 * PAD_2K + 1] = total_md.m;
    }
    __syncthreads();

    // Write the smemOutput into pTemp
    float* local_temp_buffer = pTemp + bid * PACKED_TOP_KMD_SIZE * gridDim.y + blockIdx.y * PACKED_TOP_KMD_SIZE;
#pragma unroll
    for (int i = tid; i < PACKED_TOP_KMD_SIZE; i += THREADBLOCK_SIZE)
    {
        local_temp_buffer[i] = smemOutput[i];
    }
}

template <typename T, int PAD_2K, int THREADBLOCK_SIZE, bool IS_FAST_KERNEL>
__launch_bounds__(THREADBLOCK_SIZE) __global__ void beamStage2Kernel(int* __restrict pTempId, T* __restrict pTempVal,
    float* __restrict pTemp, float const* __restrict cumLogProbs, int const nBM, int const nV, int const nVPart)
{
    constexpr int PACKED_TOP_KMD_SIZE = 2 * PAD_2K + 2;
    int const bid = blockIdx.x;
    int const tid = threadIdx.x;
    T const MAX_T_VAL = std::is_same_v<T, half> ? HALF_FLT_MAX : FLT_MAX;

    using KVPair = cub::KeyValuePair<int, T>;
    using BlockReduceTopK = cub::BlockReduce<KVPair, THREADBLOCK_SIZE>;
    using BlockReduceMD = cub::BlockReduce<MD, THREADBLOCK_SIZE>;

    __shared__ KVPair buf_smem_kv[PAD_2K];

    __shared__ union
    {
        typename BlockReduceTopK::TempStorage topk;
        typename BlockReduceMD::TempStorage md;

    } smemReduceBuffer;

    cub::ArgMax argmax;
    MD partial_md{-MAX_T_VAL, 0.0f};
    KVPair topKVPair{nV - 1, -MAX_T_VAL};

    auto reduce_md_func = [](const MD& a, const MD& b) { return reduce_md_op(a, b); };

    // Load and unpack into registers through smem
    float* localTempBuffer = pTemp + PACKED_TOP_KMD_SIZE * bid * nVPart;
    if constexpr (IS_FAST_KERNEL) // Use share memory instead of global memory
    {
        extern __shared__ char smem[];
        float* smemVal = reinterpret_cast<float*>(smem);
        for (int idx = tid; idx < PACKED_TOP_KMD_SIZE * nVPart; idx += THREADBLOCK_SIZE)
        {
            smemVal[idx] = localTempBuffer[idx];
        }
        localTempBuffer = smemVal;
        __syncthreads();
    }

    // Find the top 2K across all nVPart
    for (int k = 0; k < 2 * nBM; ++k)
    {
        KVPair topKVPairPartial{nV - 1, -MAX_T_VAL};
        // Only threads responsible for a chunk will do the computation
        if (tid < nVPart)
        {
            for (int i = 0; i < 2 * nBM; ++i)
            {
                int const current_index = tid * PACKED_TOP_KMD_SIZE + i;
                T topValue = localTempBuffer[current_index + PAD_2K];
                topKVPairPartial = argmax(topKVPairPartial, {current_index, topValue});
            }
        }

        KVPair topKVPair = BlockReduceTopK(smemReduceBuffer.topk).Reduce(topKVPairPartial, argmax);
        __syncthreads();

        if (tid == 0)
        {
            // Store kv pairs in shared mem buffer
            int temp_offset = topKVPair.key;
            int global_offset = reinterpret_cast<int*>(localTempBuffer)[temp_offset];
            topKVPair.key = global_offset;
            buf_smem_kv[k] = topKVPair;

            // Invalidate the maximum value within the chunk
            reinterpret_cast<int*>(localTempBuffer)[temp_offset] = nV - 1; // id in share memory
            localTempBuffer[temp_offset + PAD_2K] = -MAX_T_VAL;            // value in share memory
        }
        __syncthreads();
    }

    // Extract and reduce MD values across the chunks
    if (tid < nVPart)
    {
        partial_md.d = localTempBuffer[tid * PACKED_TOP_KMD_SIZE + 2 * PAD_2K];
        partial_md.m = localTempBuffer[tid * PACKED_TOP_KMD_SIZE + 2 * PAD_2K + 1];
    }
    __syncthreads();

    MD total_md = BlockReduceMD(smemReduceBuffer.md).Reduce(partial_md, reduce_md_func);

    if (tid == 0)
    {
        float d_total_log = logf(total_md.d);

        for (int i = 0; i < PAD_2K; ++i)
        {
            float val = (float) buf_smem_kv[i].value - total_md.m - d_total_log;
            if (i < 2 * nBM)
            {
                pTempId[bid * 2 * nBM + i] = buf_smem_kv[i].key;
                pTempVal[bid * 2 * nBM + i] = val + cumLogProbs[bid];
            }
        }
    }
}

#define BEAM_STAGE2_KERNEL(N_VOCAB_PART, IS_FAST_KERNEL)                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (IS_FAST_KERNEL && nShareMemory >= (48 << 10))                                                              \
        {                                                                                                              \
            TLLM_CUDA_CHECK(cudaFuncSetAttribute(beamStage2Kernel<T, PAD_2K, N_VOCAB_PART, IS_FAST_KERNEL>,            \
                cudaFuncAttributeMaxDynamicSharedMemorySize, nShareMemory));                                           \
        }                                                                                                              \
        beamStage2Kernel<T, PAD_2K, N_VOCAB_PART, IS_FAST_KERNEL>                                                      \
            <<<nBS * nBM, N_VOCAB_PART, IS_FAST_KERNEL * nShareMemory, stream>>>(                                      \
                pTempId, pTempVal, pTemp, cumLogProbs, nBM, nV, nVPart);                                               \
    } while (0);                                                                                                       \
    return;

template <typename T, int PAD_2K>
__inline__ void beamStage2KernelLauncher(float* pTemp, float const* cumLogProbs, int* pTempId, T* pTempVal,
    int const nBS, int const nBM, int const nVPart, int const nV, int const max_smem_per_block, cudaStream_t stream)
{
    // TODO: rewrite kernel to remove dependence of constant block size to reduce compilation time
    size_t const nShareMemory = sizeof(float) * nVPart * (2 * PAD_2K + 2) + sizeof(cub::KeyValuePair<int, T>) * PAD_2K;
    if (nShareMemory < max_smem_per_block) // IS_FAST_KERNEL must be a compilation-time constant
    {
        if (nVPart <= 32)
        {
            BEAM_STAGE2_KERNEL(32, true)
        }
        if (nVPart <= 64)
        {
            BEAM_STAGE2_KERNEL(64, true)
        }
        BEAM_STAGE2_KERNEL(128, true)
        // No larger branch since nVPart <= nMaxVocabPartForStage1FastKernel
    }
    BEAM_STAGE2_KERNEL(128, false)
}

template <typename T, int PAD_K>
void topKSoftMaxKernelLauncher(T const* logits, T const* bias, void* workspace, BeamHypotheses& bh, cudaStream_t stream)
{
    // Workflow of this function (reference: https://github.com/NVIDIA/online-softmax)
    // Using batch_size (BS) = 2, beam_width (BM) = 5, vocab_size (V) = vocan_size_padded (VP) = 32000 as an example:
    // nPaddedBeamWidth (PAD_K) = 2 ^ ceil(log(BM)) = 8, PAD_2K = 2 * PAD_K = 16
    // logits.shape = [BS, BM, V]
    // nBlockSize = 128, nVPart = 13, nVocabChunk = 2462 = ceil(32000/13)

    // The content of workspace (length aligned to 4):
    //             | allocated size                         | used size                | data type |
    // ┏━━━━━━━━━━┓ --------------------------------------------------------------------------------
    // ┃ pTempId  ┃ BS * PAD_K * PAD_K * 2                  |                          | int       |
    // ┣━━━━━━━━━━┫ ----------------------------------------- Change "PAD_K" into "BM" -------------
    // ┃ pTempVal ┃ BS * PAD_K * PAD_K * 2                  |                          | float     |
    // ┣━━━━━━━━━━┫ ----------------------------------------- in the left formulas     -------------
    // ┃ pTemp    ┃ BS * PAD_K * VP * (2 * (PAD_K * 2) + 2) |                          | float     |
    // ┗━━━━━━━━━━┛ --------------------------------------------------------------------------------

    // Stage1: gridDim(BS*BM,nVPart,1), blockDim(nBlockSize,1,1)
    // Each ThreadBlock takes `nVocabChunk` contiguous elements in logits to do TopK and reduce_md,
    //   then writes output into pTemp.
    // At end of this kernel, each ThreadBlock holds the indexes and values of the top 2*BM elements,
    //   as well as the m(x) and l(x) of those elements (see paper of Flash Attention, arXiv:2205.14135)
    // pTemp.shape = [BS*BM, nVPart, 2*PAD_2K+2]
    // The content of the last dimension of pTemp (updated by each ThreadBlock, we call it "Tile"):
    //                  ┏━━━━━━━━━┳━━━━━━━━━━┳━━━━━━━┓
    //                  ┃ topk_id ┃ topk_val ┃ md    ┃
    //                  ┗━━━━━━━━━┻━━━━━━━━━━┻━━━━━━━┛
    // | allocated size | PAD_2K  | PAD_2K   | 2     |
    // | used size      | BM * 2  | BM * 2   | 2     |
    // | data type      | int     | float    | float |

    // Stage2: gridDim(BS*BM,1,1), blockDim(32/64/128,1,1)
    // Each TheadBlock takes `nVPart` contiguous Tiles in pTemp to do reduce_topk and reduce_md,
    //   writes output topk_id into in pTempId, writes topk_value + cumLogProbs into pTempVal.

    // beamStage3Kernel: gridDim(BS,1,1), blockDim(128,1,1)
    // Each TheadBlock is responsible for one batch, doing work below:
    //   + moves one beam into candidate-beam-array if it is finished (gemerated end_id in this step).
    //   + selects BM elements for the next generation step if not.
    //   + maintains related score array, min_normed_score / batchDones / finished, etc..

    int constexpr items_per_thread = 1;
    int constexpr nBlockSize = (PAD_K < 16) ? ((PAD_K < 8) ? nBlockSizeForSmallBeamWidth : 128) : 64;
    int const nBS{bh.nBatchSize};
    int const nBM{bh.nBeamWidth};
    int const nV{bh.nVocabSize};
    int const* endIds{bh.endIds};
    FinishedState const* finished{bh.finished};

    int const offset = roundUp(nBS * nBM * nBM * 2, 4);
    int* pTempId = reinterpret_cast<int*>(workspace);
    T* pTempVal = reinterpret_cast<T*>(pTempId + offset);
    float* pTemp = reinterpret_cast<float*>(pTempVal + offset);

#ifdef DO_SPLIT_SMALL_TOP_K_SOFTMAX

    // Upper limit count of ThreadBlock, gotten by using no share memory
    int max_active_blocks = -1;
    TLLM_CUDA_CHECK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
        &max_active_blocks, beamStage1FastKernel<T, items_per_thread, 2 * PAD_K, nBlockSize>, nBlockSize, 0));

    // Find the max smem on the device and use that to determine the vocab parts in the best case.
    int max_smem_per_sm = -1;
    int max_smem_per_block = -1;
    int const device = tensorrt_llm::common::getDevice();
    TLLM_CUDA_CHECK(cudaDeviceGetAttribute(&max_smem_per_sm, cudaDevAttrMaxSharedMemoryPerMultiprocessor, device));
    TLLM_CUDA_CHECK(cudaDeviceGetAttribute(&max_smem_per_block, cudaDevAttrMaxSharedMemoryPerBlockOptin, device));
    cudaFuncAttributes attr;
    TLLM_CUDA_CHECK(cudaFuncGetAttributes(&attr, beamStage1FastKernel<T, items_per_thread, 2 * PAD_K, nBlockSize>));

    // One ThreadBlock must at least have share memory of `sizeof(T) * nV / nMaxVocabPartForStage1FastKernel` bytes
    int const static_smem = attr.sharedSizeBytes;
    int const max_dyn_smem_per_block = max_smem_per_block - static_smem;
    TLLM_CHECK_WITH_INFO(sizeof(T) * nV <= max_dyn_smem_per_block * nMaxVocabPartForStage1FastKernel,
        "Vocab size is too large for split-k TopK beam search fast path.");

    // Find the maximum of ThreadBlock (maximum of nVPart, minimum of smem),
    // satisfying nVPart <= nMaxVocabPartForStage1FastKernel && dyn_smem_size * nVPart >= sizeof(T) * nV
    int const driver_smem_per_block = max_smem_per_sm - max_smem_per_block;
    int const extra_smem = driver_smem_per_block + static_smem;

    int nVPart = nMaxVocabPartForStage1FastKernel + 1;
    for (int n_block = max_active_blocks - 1; n_block > 0 && nVPart > nMaxVocabPartForStage1FastKernel; --n_block)
    {
        int dyn_smem_size = max_smem_per_sm / n_block - extra_smem;
        dyn_smem_size -= dyn_smem_size % sizeof(T);
        nVPart = ceilDiv(sizeof(T) * nV, dyn_smem_size);
    }

    if (nVPart <= nMaxVocabPartForStage1FastKernel)
    {
        // Use stage 1 fast kernel
        int const nVocabChunk = (nV + nVPart - 1) / nVPart;
        int const dyn_smem_size = sizeof(T) * nVocabChunk;
        if (dyn_smem_size >= (48 << 10))
        {
            TLLM_CUDA_CHECK(cudaFuncSetAttribute(beamStage1FastKernel<T, items_per_thread, 2 * PAD_K, nBlockSize>,
                cudaFuncAttributeMaxDynamicSharedMemorySize, dyn_smem_size));
        }
        dim3 gridSize(nBS * nBM, nVPart);
        beamStage1FastKernel<T, items_per_thread, 2 * PAD_K, nBlockSize>
            <<<gridSize, nBlockSize, dyn_smem_size, stream>>>(
                logits, bias, pTemp, endIds, finished, nBM, nV, nVocabChunk);
    }
    else
    {
        // Use stage 1 base kernel, useless branch now
        int nVPart = 4;
        if (nBS * nBM < 256)
        {
            // TODO: add heuristics for base stage 1 kernel
            // Volta has 80 SMs, so we aim for three waves
            nVPart = (240 + nBS * nBM - 1) / (nBS * nBM);
            nVPart = std::min(128, nVPart); // we implement up to 128
        }
        cudaFuncSetAttribute(beamStage1BaseKernel<T, items_per_thread, 2 * PAD_K, nBlockSize>,
            cudaFuncAttributePreferredSharedMemoryCarveout, cudaSharedmemCarveoutMaxL1);
        dim3 gridSize(nBS * nBM, nVPart);
        beamStage1BaseKernel<T, items_per_thread, 2 * PAD_K, nBlockSize>
            <<<gridSize, nBlockSize, 0, stream>>>(logits, bias, pTemp, endIds, finished, nBM, nV);
    }
    sync_check_cuda_error();

    beamStage2KernelLauncher<T, 2 * PAD_K>(
        pTemp, bh.cumLogProbs, pTempId, pTempVal, nBS, nBM, nVPart, nV, max_smem_per_block, stream);

#else
    beamKernel<T, items_per_thread, PAD_K, nBlockSize>
        <<<nBS * nBM, nBlockSize, 0, stream>>>(logits, bias, pTempId, pTempVal, bh);
#endif

    sync_check_cuda_error();

    // Keep top 2K candidates in case of k candidates finishes in one iteration
    size_t const nShareMemory = sizeof(T) * nBM * nBM * 2;
    size_t constexpr nBlockSizeStage3 = (PAD_K + 31) / 32 * 32; // can not use `roundUp()`
    if (nShareMemory >= (48 << 10))
    {
        TLLM_CUDA_CHECK(cudaFuncSetAttribute(beamStage3Kernel<T, PAD_K * 2, nBlockSizeStage3>,
            cudaFuncAttributeMaxDynamicSharedMemorySize, nShareMemory));
    }

    beamStage3Kernel<T, PAD_K * 2, nBlockSizeStage3>
        <<<nBS, nBlockSizeStage3, nShareMemory, stream>>>(pTempId, pTempVal, bh);
    sync_check_cuda_error();
}

#define INSTANTIATE_BEAMSEARCH_K(T, PAD_K)                                                                             \
    template void topKSoftMaxKernelLauncher<T, PAD_K>(                                                                 \
        T const* logits, T const* bias, void* workspace, BeamHypotheses& bh, cudaStream_t stream);

} // namespace kernels
} // namespace tensorrt_llm
