# SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import torch

from tensorrt_llm.quantization import QuantAlgo

from ...._utils import str_dtype_to_torch


def shuffle_qkv_weights(weights, config):
    # Input weights are organized as
    # (q00, q01, ... q0m, k0, v0), (q10, q11, ... q1m, k1, v1), ... (qn0, qn1, ... qnm, kn, vn)
    # where n = num_kv_heads, m = num_attention_heads // num_kv_heads (i.e. #q_heads per kv_head)
    #
    # Output weights will be organized as
    # (q00, q01, ..., qnm), (k0, k1, .., kn), (v0, v1, .., vn)

    num_heads = config['num_attention_heads']
    num_kv_heads = config['num_kv_heads'] if 'num_kv_heads' in config.keys(
    ) else config['num_key_value_heads']
    num_q_per_kv = num_heads // num_kv_heads

    hidden_size = config['hidden_size']
    head_dim = hidden_size // num_heads

    input_shape = weights.shape
    if weights.dim() < 2:
        weights = weights.unsqueeze(1)

    weights = weights.reshape(num_kv_heads, (num_q_per_kv + 2), head_dim,
                              weights.shape[-1])
    q = weights[:, :-2, :, :]
    k = weights[:, -2, :, :]
    v = weights[:, -1, :, :]

    # num_heads x head_dim x hidden_size
    q = q.reshape(-1, q.shape[2], q.shape[3])

    # num_heads + (2 * num_kv_heads) x head_dim x hidden_size
    weights = torch.cat([q, k, v], dim=0)
    weights = weights.reshape(-1, weights.shape[2])

    weights = weights.squeeze()
    assert input_shape == weights.shape

    return weights


def split(v, tp_size, idx, dim=0):
    if tp_size == 1:
        return v
    if len(v.shape) == 1:
        return torch.chunk(v, tp_size)[idx].contiguous()
    else:
        return torch.chunk(v, tp_size, dim=dim)[idx].contiguous()


def split_qkv_tp(v, n_head, n_hidden, tensor_parallel, rank):
    """
    Splits the QKV matrix according to tensor parallelism
    """
    v = v.reshape(3, n_hidden, n_hidden)
    split_v = split(v, tensor_parallel, rank, dim=1)
    split_v = split_v.reshape(3 * (n_hidden // tensor_parallel), n_hidden)
    return split_v.contiguous()


def split_qkv_bias_tp(v, n_head, n_hidden, tensor_parallel, rank):
    """
    Splits the QKV bias according to tensor parallelism
    """
    v = v.reshape(3, n_hidden)
    split_v = split(v, tensor_parallel, rank, dim=1)
    split_v = split_v.reshape(3 * (n_hidden // tensor_parallel))
    return split_v.contiguous()


def split_matrix_tp(v, tensor_parallel, rank, dim):
    return split(v, tensor_parallel, rank, dim=dim)


def split_embedding(
    param: torch.Tensor,
    tp_size: int,
    tp_rank: int,
    use_parallel_embedding: bool = False,
    sharding_dim: int = 0,
) -> torch.Tensor:
    if param is None:
        return None
    if not use_parallel_embedding:
        return param

    vocab_size, hidden_size = param.size()
    if sharding_dim == 0:
        if vocab_size % tp_size != 0:
            vocab_size_padded = pad_vocab_size(vocab_size, tp_size)
            pad_width = vocab_size_padded - vocab_size
            param = torch.nn.functional.pad(param, (0, 0, 0, pad_width),
                                            value=0)
        else:
            assert hidden_size % tp_size == 0
    return split(param, tp_size, tp_rank, dim=sharding_dim)


def get_weight(config, prefix, dtype):
    return config[prefix + '.weight'].to(dtype).detach()


def get_bias(config, prefix, dtype):
    return config[prefix + '.bias'].to(dtype).detach()


def get_weight_and_bias(config, prefix, dtype):
    return get_weight(config, prefix, dtype), get_bias(config, prefix, dtype)


def get_tllm_linear_weight(weight,
                           prefix,
                           bias=None,
                           use_weight_only=False,
                           plugin_weight_only_quant_type=torch.int8):
    results = {}
    if use_weight_only:
        v = weight.t().contiguous()
        processed_torch_weights, torch_weight_scales = \
            torch.ops.trtllm.symmetric_quantize_last_axis_of_batched_matrix(
                v, plugin_weight_only_quant_type)
        results[prefix + '.weight'] = processed_torch_weights
        results[prefix + '.per_channel_scale'] = torch_weight_scales
    else:
        results[prefix + '.weight'] = weight.contiguous()

    if bias is not None:
        results[prefix + '.bias'] = bias

    return results


def split_weights_tp(config, weights, args, rank, dtype):
    num_heads = config['num_attention_heads']
    num_kv_heads = config['num_kv_heads']
    hidden_size = config['hidden_size']

    mha_mode = num_heads == num_kv_heads
    tp_size = args.tp_size

    use_weight_only = args.use_weight_only
    plugin_weight_only_quant_type = None
    if use_weight_only and args.weight_only_precision == 'int8':
        plugin_weight_only_quant_type = torch.int8
    elif use_weight_only and args.weight_only_precision == 'int4':
        plugin_weight_only_quant_type = torch.quint4x2

    # Helper
    def get_weight(weight, prefix, bias):
        return get_tllm_linear_weight(weight, prefix, bias, use_weight_only,
                                      plugin_weight_only_quant_type)

    for layer_id in range(config['num_hidden_layers']):
        layer_prefix = f"transformer.layers.{layer_id}."

        prefix = layer_prefix + 'attention.qkv'
        qkv_weight, qkv_bias = get_weight_and_bias(weights, prefix, dtype)

        if not mha_mode:
            num_q_per_kv = num_heads // num_kv_heads

            qkv_weight = qkv_weight.reshape(num_q_per_kv + 2, -1, hidden_size)
            q = qkv_weight[:num_q_per_kv, :, :].reshape(-1, hidden_size)
            k = qkv_weight[num_q_per_kv:num_q_per_kv + 1, :, :].reshape(
                -1, hidden_size)
            v = qkv_weight[num_q_per_kv + 1:num_q_per_kv + 2, :, :].reshape(
                -1, hidden_size)
            split_weight = torch.cat(
                [split(x, tp_size, rank) for x in [q, k, v]], dim=0)

            qkv_bias = qkv_bias.reshape(num_q_per_kv + 2, -1)
            q = qkv_bias[:num_q_per_kv, :].reshape(-1)
            k = qkv_bias[num_q_per_kv:num_q_per_kv + 1, :].reshape(-1)
            v = qkv_bias[num_q_per_kv + 1:num_q_per_kv + 2, :].reshape(-1)
            split_bias = torch.cat([split(x, tp_size, rank) for x in [q, k, v]],
                                   dim=0)
        else:
            split_weight = split_qkv_tp(qkv_weight, num_heads, hidden_size,
                                        tp_size, rank)
            split_bias = split_qkv_bias_tp(qkv_bias, num_heads, hidden_size,
                                           tp_size, rank)

        weights.update(get_weight(split_weight, prefix, split_bias))

        prefix = layer_prefix + 'attention.dense'
        attn_dense_weight, attn_dense_bias = get_weight_and_bias(
            weights, prefix, dtype)
        split_v = split_matrix_tp(attn_dense_weight, tp_size, rank, dim=1)
        weights.update(get_weight(split_v, prefix, attn_dense_bias))

        prefix = layer_prefix + 'mlp.fc'
        mlp_fc_weight, mlp_fc_bias = get_weight_and_bias(weights, prefix, dtype)
        split_v = split_matrix_tp(mlp_fc_weight, tp_size, rank, dim=0)
        bias = split_matrix_tp(mlp_fc_bias, tp_size, rank, dim=0)
        weights.update(get_weight(split_v, prefix, bias))

        prefix = layer_prefix + 'mlp.proj'
        mlp_proj_weight, mlp_proj_bias = get_weight_and_bias(
            weights, prefix, dtype)
        split_v = split_matrix_tp(mlp_proj_weight, tp_size, rank, dim=1)
        weights.update(get_weight(split_v, prefix, mlp_proj_bias))

    weights['transformer.vocab_embedding.weight'] = split_embedding(
        weights['transformer.vocab_embedding.weight'], tp_size, rank)
    weights['lm_head.weight'] = split_matrix_tp(weights['lm_head.weight'],
                                                tp_size,
                                                rank,
                                                dim=0)

    return weights


def convert_hf_weights(hf_model, config, args, rank):
    torch_dtype = str_dtype_to_torch(args.dtype)
    hf_state_dict = hf_model.state_dict()
    weights = {}

    # replace key name
    for key, value in hf_state_dict.items():
        # Decoder Layers
        if "model.layers." in key:
            key = key.replace("model.layers.", "transformer.layers.")
            key = key.replace("self_attn.", "attention.")
            key = key.replace("query_key_value.", "qkv.")
            key = key.replace("mlp.up_proj.", "mlp.fc.")
            key = key.replace("mlp.down_proj.", "mlp.proj.")
            key = key.replace("post_attention_layernorm.", "post_layernorm.")
        # Embedding
        key = key.replace("model.embed_tokens.weight",
                          "transformer.vocab_embedding.weight")
        # Final Layer norm
        key = key.replace("model.final_layernorm.", "transformer.ln_f.")
        weights[key] = value.to(torch_dtype).cpu()

    weights['lm_head.weight'] = weights[
        'transformer.vocab_embedding.weight'].clone()

    # Transform QKV weights from custom Phi3Small format to TRT-LLM format
    for key, value in weights.items():
        if "qkv." in key:
            weights[key] = shuffle_qkv_weights(weights[key], config)

    weights = split_weights_tp(config, weights, args, rank, torch_dtype)

    return weights


def convert_hf_config(hf_config, dtype, args):
    config = {
        'architecture': 'Phi3SmallForCausalLM',
        'dtype': dtype,
        'num_hidden_layers': hf_config.num_hidden_layers,
        'num_attention_heads': hf_config.num_attention_heads,
        'num_kv_heads': hf_config.num_key_value_heads,
        'rotary_embedding_base': hf_config.rope_embedding_base,
        'hidden_size': hf_config.hidden_size,
        'intermediate_size': hf_config.intermediate_size,
        'vocab_size': hf_config.vocab_size,
        'max_position_embeddings': hf_config.max_position_embeddings,
        'hidden_act': hf_config.hidden_act,
        'share_embedding_table': False,
        'gegelu_limit': hf_config.gegelu_limit,
        'mup_attn_multiplier': hf_config.mup_attn_multiplier,
        'mup_embedding_multiplier': hf_config.mup_embedding_multiplier,
        'mup_use_scaling': hf_config.mup_use_scaling,
        'mup_width_multiplier': hf_config.mup_width_multiplier,
        'blocksparse_block_size': hf_config.blocksparse_block_size,
        'blocksparse_homo_head_pattern':
        hf_config.blocksparse_homo_head_pattern,
        'blocksparse_num_local_blocks': hf_config.blocksparse_num_local_blocks,
        'blocksparse_vertical_stride': hf_config.blocksparse_vert_stride,
        'dense_attention_every_n_layers':
        hf_config.dense_attention_every_n_layers,
    }

    if args is not None:
        config.update({
            'mapping': {
                'world_size': args.tp_size * args.pp_size,
                'tp_size': args.tp_size,
                'pp_size': args.pp_size,
            }
        })

        if args.use_weight_only and args.weight_only_precision == 'int8':
            config.update({'quantization': {'quant_algo': QuantAlgo.W8A16}})
        elif args.use_weight_only and args.weight_only_precision == 'int4':
            config.update({'quantization': {'quant_algo': QuantAlgo.W4A16}})

    if hf_config.max_position_embeddings >= 128000:
        config.update({
            'original_max_position_embeddings':
            hf_config.original_max_position_embeddings,
            'longrope_scaling_short_factors':
            hf_config.rope_scaling["short_factor"],
            'longrope_scaling_long_factors':
            hf_config.rope_scaling["long_factor"],
            'longrope_long_mscale':
            hf_config.rope_scaling["long_mscale"],
            'longrope_short_mscale':
            hf_config.rope_scaling["short_mscale"]
        })
    return config
