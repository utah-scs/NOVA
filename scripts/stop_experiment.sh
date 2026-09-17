#!/bin/bash

# Script for stopping the experiment
# from client side

SCRIPT_DIR="$(dirname "$(realpath "$0")")"
BESSCTL="$(realpath "$SCRIPT_DIR/../bessctl/bessctl")"

# Stop the experiment on host
"$BESSCTL" daemon stop

# Stop the experiment on the DPU
ssh ubuntu@192.168.100.2 "~/NOVA/bessctl/bessctl daemon stop"
