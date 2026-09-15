#!/bin/bash

# Script directory
SCRIPT_DIR=$(dirname "$(realpath "$0")")


while true; do
  ${SCRIPT_DIR}/run_exp.py -e FAULT_TOLERANCE/ -c FAULT_TOLERANCE/crash_bess.bess -n 1

  ${SCRIPT_DIR}/../bessctl/bessctl monitor port
done
