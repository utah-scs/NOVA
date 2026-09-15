#!/bin/bash

# Script directory 
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
bessctl_bin="${script_dir}/../bessctl/bessctl"
config_file="${script_dir}/CLIENT_REG_MULTINODE/exp_tenant_scale_naam.bess"
bpf_src="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.c"

num_cpu=$1
num_functions=$2
bessd_m=$3
bessd_buffers=$4

if [ -z "$num_functions" ]; then
  echo "Usage: $0 <num_functions>"
  exit 1
fi

cmd=''
for i in $(seq 0 $((${num_functions}-1))); do
  cmd+="-b ${bpf_src} "
done

./run_exp.py -c CLIENT_REG_MULTINODE/exp_tenant_scale_naam.bess $cmd -j -n ${num_cpu} -o="-m ${bessd_m} -buffers ${bessd_buffers}" &> /dev/null
${bessctl_bin} daemon disconnect &> /dev/null && echo "NAAM server started with ${num_functions} functions" || echo "Failed to start server"
