#!/bin/bash

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

function usage () {
  echo "usage: $(basename $0) [deps | hugepage | grpc | pkgconfig | doca | build_bess | remote_build_x86 | remote_build_arm | all]"
}

# Install necessary packages
function install_deps () {
  sudo apt update

  sudo apt -y install make \
    apt-transport-https \
    ca-certificates \
    g++ \
    clang \
    curl zip unzip tar meson cmake \
    autoconf \
    libtool \
    git \
    python3 \
    python3-pip \
    pkg-config \
    libunwind8-dev \
    liblzma-dev \
    zlib1g-dev \
    libpcap-dev \
    libssl-dev \
    libnuma-dev \
    libjson-c-dev \
    python3-scapy \
    libgflags-dev \
    libgoogle-glog-dev \
    libgraph-easy-perl \
    libgtest-dev \
    libc-ares-dev \
    libbenchmark-dev \
    libgtest-dev \
    linux-tools-common linux-tools-generic linux-tools-`uname -r` \
    libgrpc++-dev libprotobuf-dev \
    protobuf-compiler-grpc

  ARCH=$(uname -m)

  if [[ $ARCH == "x86_64" ]]; then
      sudo apt -y install gcc-multilib
  fi

  pip3 install --break-system-packages --upgrade pip

  # The following packages are needed to run bessctl
  # --break-system-packages is required on Ubuntu 24.04+ (PEP 668)
  pip3 install --break-system-packages protobuf==3.20.0 grpcio scapy
}

# Fetch all submodules
function update_submodule() {
  git submodule update --init --recursive
}

# Install DOCA framework
function install_doca () {
  if [[ $ARCH == "x86_64" ]]; then
    ${SCRIPT_DIR}/doca-host-setup.sh --all
  fi
}

function setup_hugepage() {
  for node in /sys/devices/system/node/node*; do
    echo 1024 | sudo tee ${node}/hugepages/hugepages-2048kB/nr_hugepages
  done
}

# Ensure gRPC pkgconfig files are visible under /usr/local/lib/pkgconfig.
# gRPC is installed system-wide via apt; we create symlinks so that
# /usr/local/lib/pkgconfig (prepended to PKG_CONFIG_PATH by the build) finds them.
function install_grpc () {
  local arch_triplet
  arch_triplet="$(uname -m)-linux-gnu"
  local sys_pc_dir="/usr/lib/${arch_triplet}/pkgconfig"
  local local_pc_dir="/usr/local/lib/pkgconfig"

  if ! pkg-config --exists grpc++; then
    echo "ERROR: gRPC++ not found. Install with: sudo apt install -y libgrpc++-dev" >&2
    return 1
  fi

  sudo mkdir -p "${local_pc_dir}"
  for pc in grpc.pc grpc++.pc grpc_unsecure.pc grpc++_unsecure.pc; do
    if [[ -f "${sys_pc_dir}/${pc}" && ! -e "${local_pc_dir}/${pc}" ]]; then
      sudo ln -sf "${sys_pc_dir}/${pc}" "${local_pc_dir}/${pc}"
    fi
  done
  echo "gRPC pkgconfig symlinks created in ${local_pc_dir}"
}

# Symlink system protobuf.pc into /usr/local/lib/pkgconfig so the build's
# PKG_CONFIG_PATH (which starts with /usr/local/lib/pkgconfig) finds it.
function setup_pkgconfig () {
  local arch_triplet
  arch_triplet="$(uname -m)-linux-gnu"
  local sys_proto_pc="/usr/lib/${arch_triplet}/pkgconfig/protobuf.pc"
  local pkgconfig_dir="/usr/local/lib/pkgconfig"

  if [[ ! -f "${sys_proto_pc}" ]]; then
    echo "ERROR: ${sys_proto_pc} not found." >&2
    echo "       Install with: sudo apt install -y libprotobuf-dev" >&2
    return 1
  fi

  sudo mkdir -p "${pkgconfig_dir}"
  sudo ln -sf "${sys_proto_pc}" "${pkgconfig_dir}/protobuf.pc"
  echo "Created ${pkgconfig_dir}/protobuf.pc -> ${sys_proto_pc}"
}

function build_bess () {
  local arch_triplet
  arch_triplet="$(uname -m)-linux-gnu"
  export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/opt/mellanox/doca/lib/${arch_triplet}/pkgconfig:/opt/mellanox/dpdk/lib/${arch_triplet}/pkgconfig:/opt/mellanox/flexio/lib/pkgconfig

  # First remove old compilation if any
  ./build.py dist_clean

  # Build bess
  ./build.py bess --plugin dma_plugin -v
}

function remote_build_bess_x86 {
  export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/opt/mellanox/doca/lib/x86_64-linux-gnu/pkgconfig:/opt/mellanox/grpc/lib/pkgconfig:/opt/mellanox/dpdk/lib/x86_64-linux-gnu/pkgconfig

  # First remove old compilation if any
  ./build.py dist_clean

  # Build bess
  ./build.py bess --plugin dma_plugin && echo "Build success" || echo "Build failed"
}

function remote_build_bess_arm {
  export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:/opt/mellanox/doca/lib/aarch64-linux-gnu/pkgconfig:/opt/mellanox/grpc/lib/pkgconfig:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig

  # First remove old compilation if any
  ./build.py dist_clean

  # Build bess
  ./build.py bess --plugin dma_plugin && echo "Build success" || echo "Build failed"
}

if [[ $# -ne 1 ]]; then
  usage
  exit 1
fi

# Parse command line arguments
case "$1" in
  deps)
    install_deps
    ;;
  submodule)
    update_submodule
    ;;
  grpc)
    install_grpc
    ;;
  pkgconfig)
    setup_pkgconfig
    ;;
  hugepage)
    setup_hugepage
    ;;
  doca)
    install_doca
    ;;
  build_bess)
    build_bess
    ;;
  remote_build_x86)
    remote_build_bess_x86
    ;;
  remote_build_arm)
    remote_build_bess_arm
    ;;
  all)
    install_deps
    update_submodule
    install_grpc
    setup_pkgconfig
    build_bess
    ;;
  *)
    usage
    exit 1
    ;;
esac
