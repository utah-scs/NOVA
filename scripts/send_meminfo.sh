#!/usr/bin/env bash

# Script for sending exported meminfo to DPU
# First start the host bessd and then run this script
# to send the meminfo to DPU

scp -o StrictHostKeyChecking=no /tmp/meminfo_1.txt ubuntu@192.168.100.2:/tmp/
scp -o StrictHostKeyChecking=no /tmp/exportdata_1.bin ubuntu@192.168.100.2:/tmp/
