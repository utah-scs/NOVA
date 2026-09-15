#!/bin/bash

# Script directory 
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
bessctl_bin="${script_dir}/../bessctl/bessctl"
config_file="${script_dir}/CLIENT_REG_MULTINODE/exp_tenant_scale_ipipe.bess"
bpf_src="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.c"
bpf_obj="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.o"

num_cpu=$1
num_functions=$2
bess_mem=$3
bess_buffers=$4

ARCH=$(uname -m)

if [ -z "$num_functions" ]; then
  echo "Usage: $0 <num_functions>"
  exit 1
fi

# Clean up leftover state from a previous run. A bessd instance that
# dies uncleanly (crash, kill -9) leaves its DPDK hugepage segments
# (rte<N>map_*) and DMA regions (dma_shm_*) behind, which permanently
# eats into the node's hugepage pool and starves rte_eal_init() for
# later tenants.
echo "Cleaning up leftover BESS state..."
sudo pkill -9 -f "bessd -grpc_url" 2>/dev/null
sleep 1
sudo rm -f /dev/hugepages/rte*map_* /mnt/huge/dma_shm_*
rm -rf /tmp/bessd*
# Restore the terminal in case a previous run left it in a bad state (see
# below for why that happens).
stty sane 2>/dev/null || true

# Compile the bpf code
clang -I ${script_dir}/../deps/ubpf/vm/inc/ -I /usr/include/aarch64-linux-gnu/ -target bpf -O2 -c $bpf_src -o $bpf_obj &> /dev/null

for i in $(seq 0 $((${num_functions}-1))); do
  port=$((10515+$i))
  # prefix="rte$i"
  # All tenants share one DPDK multi-process domain (same -file_prefix) so
  # that tenant 0 comes up as the DPDK primary (owns/probes the PMD port)
  # and every later tenant auto-detects that primary and attaches as a
  # secondary sharing the same port, instead of each tenant trying to
  # become its own primary and fighting over the same PMD_PORT_ID.
  prefix="rte0"

  if [ "$ARCH" == "aarch64" ]; then
    cpu_id=$(($i % ${num_cpu}))
  else
    cpu_id=$((($i % ${num_cpu}) * 2))
  fi
  
  q_id=$i

  if [ -e /tmp/bessd${i} ]; then
    rm -rf /tmp/bessd${i}
  fi
  
  mkdir -p /tmp/bessd$i
  
  # NOTE: In case of environmen key/value pairs values need
  # to be valid python data types. For example, True/False, "string", 0 etc.
  #
  # stdin is redirected from /dev/null: bessctl's "daemon start" launches
  # bessd via sudo, and this cluster's sudoers has "use_pty" on, so sudo
  # allocates a pty for bessd and relays it against whatever bessctl's
  # stdin is. bessd daemonizes (forks into the background, ppid -> 1) but
  # keeps that pty fd open as a long-lived background process instead of
  # closing it, so if bessctl inherited our real terminal here, the
  # terminal is left desynced from the now-orphaned pty relay once the
  # foreground sudo invocation returns -- symptom: the shell stops
  # echoing typed characters. Not inheriting a real tty avoids this.
  echo "Starting ipipe${i}"
  $bessctl_bin daemon start -grpc_url 127.0.0.1:${port} -log_dir /tmp/bessd${i} -file_prefix ${prefix} -proc_type auto -m ${bess_mem} -buffers ${bess_buffers} < /dev/null
  $bessctl_bin daemon connect 127.0.0.1:${port} -- run file $config_file 'enable_jit=True, bpf_obj="'$bpf_obj'", num_queues='$num_functions', cpu_id='$cpu_id', q_id='$q_id'' < /dev/null
done

# Defensive reset in case anything along the way still left the terminal
# in a bad (e.g. no-echo) state.
stty sane 2>/dev/null || true

echo "iPipe server started with $num_functions functions"
