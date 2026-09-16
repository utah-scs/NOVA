#!/bin/bash

##################################
# NAAM B+ Tree Experiment Script #
##################################

# Start the server first then run this script to get tput/latency

# Usage: ./run_bpt_exp.sh -o <output.csv> -b <bench>

while getopts "o:b:" opt; do
	case "$opt" in
		o) CSV_FILE=$(readlink -f "$OPTARG") ;;
		b) BENCH="$OPTARG" ;;
		*) echo "Usage: $0 -o <output.csv> -b <bench>"; exit 1 ;;
	esac
done

if [ -z "$CSV_FILE" ] || [ -z "$BENCH" ]; then
	echo "Usage: $0 -o <output.csv> -b <bench>"
	exit 1
fi

# Absolute path of the script directory
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

# Client binary, resolved relative to the script directory so this script
# can be invoked from anywhere
CLIENT_BIN="${SCRIPT_DIR}/../client/build/client"

# Directory where experiment logs are stored, same directory as the CSV file
RESULTS_DIR=$(dirname "$CSV_FILE")
mkdir -p "$RESULTS_DIR"

SERVER_MAC=c4:70:bd:a0:59:7e

PPS_START=250000
PPS_END=500000
PPS_STEP=250000
NUM_ITER=2

LOG_FILE="${RESULTS_DIR}/bpt_experiment.log"
rm -f "$LOG_FILE"

echo "bench,pps,avg_tput_mpps,avg_lat_99th" > "$CSV_FILE"

while [ $PPS_START -le $PPS_END ]; do
	echo "Running experiment with PPS: $PPS_START"
	for i in $(seq 1 $NUM_ITER); do
		sudo "$CLIENT_BIN" -l 0,2 -- -s 192.168.1.2 -M "$SERVER_MAC" -p 10002 -t 10 -T bpt -b 32 -n 1 -m fixed -r $PPS_START --src-port-start 1024 --src-port-end 1031 &>> "$LOG_FILE"
	done
	avg_tput=$(cat $LOG_FILE | grep Mpps | grep Received | awk '{total += $5}END{print total/NR}')
	avg_lat=$(cat $LOG_FILE | grep 99th | awk '{total += $4}END{print total/NR}')
	echo "Average tput,lat for PPS $PPS_START: $avg_tput, $avg_lat"
	echo "${BENCH},${PPS_START},${avg_tput},${avg_lat}" >> "$CSV_FILE"
	PPS_START=$((PPS_START + PPS_STEP))
	rm "$LOG_FILE"
done
