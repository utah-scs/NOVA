#!/bin/bash

##################################
# NAAM B+ Tree Experiment Script #
##################################

# Start the server first then run this script to get tput/latency

# Absolute path of the script directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# Client binary, resolved relative to the script directory so this script
# can be invoked from anywhere
CLIENT_BIN="${SCRIPT_DIR}/../client/build/client"

# Directory where experiment logs are stored
RESULTS_DIR="${SCRIPT_DIR}/results/bplus-tree"
mkdir -p "$RESULTS_DIR"

PPS_START=250000
PPS_END=500000
PPS_STEP=250000
NUM_ITER=5

LOG_FILE="${RESULTS_DIR}/bpt_experiment.log"
rm -f "$LOG_FILE"

while [ $PPS_START -le $PPS_END ]; do
	echo "Running experiment with PPS: $PPS_START"
	for i in $(seq 1 $NUM_ITER); do
		sudo "$CLIENT_BIN" -l 0,2 -- -s 192.168.1.2 -M c4:70:bd:a0:59:7e -p 10002 -t 10 -T bpt -b 32 -n 1 -m fixed -r $PPS_START --src-port-start 1024 --src-port-end 1031 &>> "$LOG_FILE"
	done
	avg_tput=$(cat $LOG_FILE | grep Mpps | awk '{total += $5}END{print total/NR}')
	avg_lat=$(cat $LOG_FILE | grep 99th | awk '{total += $4}END{print total/NR}')
	echo "Average tput,lat for PPS $PPS_START: $avg_tput, $avg_lat"
	PPS_START=$((PPS_START + PPS_STEP))
	rm "$LOG_FILE"
done
