#!/bin/bash

# Script directory 
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
bessctl_bin="${script_dir}/../bessctl/bessctl"
config_file="${script_dir}/CLIENT_REG_MULTINODE/exp_tenant_scale_ipipe.bess"
bpf_src="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.c"
bpf_obj="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.o"
num_functions=$1

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
clang -I ${script_dir}/../deps/ubpf/vm/inc/ -I /usr/include/aarch64-linux-gnu/ -target bpf -O2 -c $bpf_src -o $bpf_obj

for i in $(seq 0 $((${num_functions}-1))); do
  port=$((10515+$i))
  prefix="rte$i"
  cpu_id=$((($i % 8) * 2))
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
  $bessctl_bin daemon start -grpc_url 127.0.0.1:${port} -log_dir /tmp/bessd${i} -file_prefix ${prefix} -proc_type auto < /dev/null
  $bessctl_bin daemon connect 127.0.0.1:${port} -- run file $config_file 'enable_jit=True, bpf_obj="'$bpf_obj'", num_queues='$num_functions', cpu_id='$cpu_id', q_id='$q_id'' < /dev/null
done

# Defensive reset in case anything along the way still left the terminal
# in a bad (e.g. no-echo) state.
stty sane 2>/dev/null || true
