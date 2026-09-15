#!/bin/bash

# Script directory (resolved so the script works regardless of the caller's cwd)
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
bessctl_bin="${script_dir}/../bessctl/bessctl"
run_exp_bin="${script_dir}/run_exp.py"
config_file="${script_dir}/CLIENT_REG_MULTINODE/exp_tenant_scale_naam.bess"
bpf_src="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.c"

num_cpu=$1
num_functions=$2
bessd_m=$3
bessd_buffers=$4

if [ -z "$num_functions" ]; then
  echo "Usage: $0 <num_cpu> <num_functions> <bessd_m> <bessd_buffers>"
  exit 1
fi

cmd=''
for i in $(seq 0 $((${num_functions}-1))); do
  cmd+="-b ${bpf_src} "
done

"${run_exp_bin}" -c "${config_file}" $cmd -j -n ${num_cpu} -o="-m ${bessd_m} -buffers ${bessd_buffers}" &> /dev/null

# Verify the daemon is actually up (and the pipeline loaded) before declaring success.
# (compiling the eBPF programs + starting bessd can take a while, so allow up to ~30s)
daemon_ok=0
for i in $(seq 1 60); do
  if pgrep -x bessd >/dev/null 2>&1 && "${bessctl_bin}" show pipeline >/dev/null 2>&1; then
    daemon_ok=1
    break
  fi
  sleep 0.5
done

if [ "$daemon_ok" -eq 1 ]; then
  echo "NAAM server started with ${num_functions} functions"
  "${bessctl_bin}" daemon disconnect &> /dev/null
  exit 0
else
  echo "Failed to start server: bessd is not running or the pipeline did not load"
  exit 1
fi
