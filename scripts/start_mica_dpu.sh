#!/bin/bash

# Script for starting the experiment
# from client side

NUM_KEYS="64000000"
[[ "$1" == "--num-keys" && -n "$2" ]] && NUM_KEYS="$2"

# Run the experiment on DPU
ssh ubuntu@192.168.100.2 "pushd ~/NOVA/experiments && ./run_exp.py -e MICA_MULTI -c MICA_MULTI/server_simple_dpu.bess -b MICA_MULTI/mica-naam.c -j -n 6 --num-keys ${NUM_KEYS} && popd"
