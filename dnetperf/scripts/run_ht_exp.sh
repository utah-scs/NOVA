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

LCORES=32,33,34,35,36,37,38,39,40,41

# Directory where experiment logs are stored, same directory as the CSV file
RESULTS_DIR=$(dirname "$CSV_FILE")
mkdir -p "$RESULTS_DIR"

SERVER_MAC=b8:3f:d2:54:8e:fe

PPS_START=250000
PPS_END=10000000
PPS_STEP=250000
NUM_ITER=5

# A point is considered "saturated" if received throughput grows by less
# than this percentage over the previous point. Once saturation is first
# detected, we keep collecting SATURATION_EXTRA_POINTS more points (to
# confirm the plateau) and then stop early, unless PPS_END is reached first.
SATURATION_THRESHOLD_PCT=2
SATURATION_EXTRA_POINTS=3

LOG_FILE="${RESULTS_DIR}/ht_experiment.log"
rm -f "$LOG_FILE"

echo "bench,pps,avg_tput_mpps,avg_lat_99th" > "$CSV_FILE"

prev_tput=""
saturated=false
extra_points=0

while [ $PPS_START -le $PPS_END ]; do
	echo "Running experiment with PPS: $PPS_START"
	for i in $(seq 1 $NUM_ITER); do
		sudo "$CLIENT_BIN" -l $LCORES -- -s 192.168.1.2 -M "$SERVER_MAC" -p 10002 -t 10 -T ht --workload C --key-dist uniform -b 32 -n 1 -m fixed -r $PPS_START --src-port-start 1234 --src-port-end 1243 &>> "$LOG_FILE"
	done
	avg_tput=$(cat $LOG_FILE | grep Mpps | grep Received | awk '{total += $5}END{print total/NR}')
	avg_lat=$(cat $LOG_FILE | grep 99th | awk '{total += $4}END{print total/NR}')
	echo "Average tput,lat for PPS $PPS_START: $avg_tput, $avg_lat"
	echo "${BENCH},${PPS_START},${avg_tput},${avg_lat}" >> "$CSV_FILE"
	rm "$LOG_FILE"

	if [ -n "$prev_tput" ]; then
		is_saturated=$(awk -v cur="$avg_tput" -v prev="$prev_tput" -v thresh="$SATURATION_THRESHOLD_PCT" \
			'BEGIN{ if (prev+0 <= 0) { print 0; exit } growth = (cur-prev)/prev*100; print (growth < thresh) ? 1 : 0 }')
		if [ "$is_saturated" -eq 1 ]; then
			if ! $saturated; then
				echo "Throughput saturated at PPS $PPS_START (avg_tput=$avg_tput Mpps)"
				saturated=true
			fi
		else
			saturated=false
			extra_points=0
		fi
	fi

	if $saturated; then
		extra_points=$((extra_points + 1))
		if [ $extra_points -gt $SATURATION_EXTRA_POINTS ]; then
			echo "Collected $SATURATION_EXTRA_POINTS points after saturation, stopping."
			break
		fi
	fi

	prev_tput=$avg_tput
	PPS_START=$((PPS_START + PPS_STEP))
done
