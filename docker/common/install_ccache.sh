#!/bin/bash

set -ex

ARCH=$(uname -m)
CCACHE_VERSION="4.8.3"
SYSTEM_ID=$(grep -oP '(?<=^ID=).+' /etc/os-release | tr -d '"')

# 设置代理服务器地址
PROXY_SERVER="http://127.0.0.1:7890"

if [[ $ARCH == *"x86_64"* ]] && [[ $SYSTEM_ID == *"centos"* ]]; then
  http_proxy=${PROXY_SERVER} https_proxy=${PROXY_SERVER} curl -L https://github.com/ccache/ccache/releases/download/v${CCACHE_VERSION}/ccache-${CCACHE_VERSION}-linux-${ARCH}.tar.xz | xz -d | tar -x -C /tmp/
  cp /tmp/ccache-${CCACHE_VERSION}-linux-x86_64/ccache /usr/bin/ccache
fi
