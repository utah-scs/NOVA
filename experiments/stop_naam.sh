#!/bin/bash

# Script directory 
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
bessctl_bin="${script_dir}/../bessctl/bessctl"
config_file="${script_dir}/CLIENT_REG_MULTINODE/exp_tenant_scale_naam.bess"
bpf_src="${script_dir}/CLIENT_REG_MULTINODE/dma_read_write.c"

${bessctl_bin} daemon stop &> /dev/null && echo "Stopped BESS daemon" || echo "Failed to stop BESS daemon"
