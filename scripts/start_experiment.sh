#!/bin/bash

# Script for starting the experiment
# from client side

# Run the experiment on host
pushd ./experiments && ./run_exp.py -e MICA_MULTI -c server_simple_host_sep.bess -f bpf-get.c -f bpf-set.c -j && popd
./send_meminfo.sh

# Initialize eSwitch rules
ssh ubuntu@192.168.100.2 "cd ~/bess-nm/; ./switchctl.sh split init"

# Run the experiment on DPU
ssh ubuntu@192.168.100.2 "pushd ~/bess-nm/experiments && ./run_exp.py -e MICA_MULTI -c server_simple_dpu_sep.bess -f bpf-get.c -j && popd"
