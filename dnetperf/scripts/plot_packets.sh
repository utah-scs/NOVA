#!/bin/bash

# Script for plotting rtt and tput graph
# for all the csv files in a directory

set -euo pipefail

DIR="$1"

NUM_SAMPLES=3
NUM_CORES=6
OFFERED_LOAD_START=1000000
OFFERED_LOAD_END=3000000
OFFERED_LOAD_STEP=500000
OFFERED_LOAD_TRIGGER_TIME=5
BENCH_TIME=30

if [ -z "$DIR" ]; then
	echo "Usage: $0 <directory>"
	exit 1
fi

if [ ! -d "$DIR" ]; then
	mkdir -p "$DIR"
fi

# Run for N times
run_experiment_fixed() {
	for i in $(seq 1 $NUM_SAMPLES); do
		echo "Running for $i time"
		sudo client/build/client -l 0 --socket-mem=128 -- 192.168.1.2 10002 ${BENCH_TIME} set 357 32 fixed ${OFFERED_LOAD_START} ${DIR}/set_${i}_fixed_${OFFERED_LOAD_START}.csv &> ${DIR}/set_${i}_fixed_${OFFERED_LOAD_START}.txt
		sleep 5
	done
}

run_experiment_variable() {
	sudo client/build/client -l 0 --socket-mem=128 -- 192.168.1.2 10002 ${BENCH_TIME} set 357 32 variable ${OFFERED_LOAD_START} ${OFFERED_LOAD_END} ${OFFERED_LOAD_STEP} ${OFFERED_LOAD_TRIGGER_TIME} ${DIR}/set_variable.csv &> ${DIR}/set_variable.txt
}

# Normal graphs
# Arguments: bench_time, start_time, end_time, step
graph_full() {
	TIME=$1
	START_TIME=$2
	END_TIME=$3
	STEP=$4
	#TITLE="Split traffic 10/90 @10s 20/80 @20s 30/70 @30s (2 Mpps, $NUM_CORES Cores)"
	#TITLE="Split traffic 1/2 @10s 1/4 @20s 1/8 @30s (2 Mpps, $NUM_CORES Cores)"
	TITLE="Install same rule in 1s interval (2 Mpps, $NUM_CORES Cores)"
	#TITLE="Alternate rule in 1s interval (2 Mpps, $NUM_CORES Cores)"
	FILE_NAME_APPEND=''

	for file in $DIR/*.csv; do
		echo "Processing $file" 
		./plot_packets_rtt.py "$file" "$FILE_NAME_APPEND" "$TITLE" "$TIME" "$START_TIME" "$END_TIME" "$STEP"
		./plot_packets_tput.py "$file" "$FILE_NAME_APPEND" "$TITLE" "$TIME" "$START_TIME" "$END_TIME" "$STEP"
	done
}

# Zoomed graphs
# Arguments: bench_time, start_time, end_time, step
graph_zoom_func() {
	TIME=$1
	START_TIME=$2
	END_TIME=$3
	STEP=$4
	#TITLE="Split traffic 1/2 @10s 1/4 @20s 1/8 @30s (2 Mpps, $NUM_CORES Cores, zoomed @${START_TIME}s)"
	#TITLE="Split traffic 10/90 @10s 20/80 @20s 30/70 @30s (2 Mpps, $NUM_CORES Cores, zoomed @${START_TIME}s)"
	TITLE="Install same rule in 1s interval (2 Mpps, $NUM_CORES Cores, zoomed @${START_TIME}s)"
	FILE_NAME_APPEND="_zoomed_${START_TIME}s"

	for file in $DIR/*.csv; do
		echo "Processing $file" 
		./plot_packets_rtt.py "$file" "$FILE_NAME_APPEND" "$TITLE" "$TIME" "$START_TIME" "$END_TIME" "$STEP"
		./plot_packets_tput.py "$file" "$FILE_NAME_APPEND" "$TITLE" "$TIME" "$START_TIME" "$END_TIME" "$STEP"
	done
}

graph_zoom() {
	bench_time=$1
	start_time=$2
	end_time=$3
	graph_window=$4
	step=$5
	resolution=$6
	
	while [ $start_time -le $end_time ]; do
		graph_zoom_func $bench_time $start_time $((start_time + graph_window)) $resolution
		start_time=$((start_time + step))
	done
}

cleanup() {
	# Remove all the data files
	find $DIR -name "*.csv" -type f -delete

	# Move graphs to seperate directory
	mkdir -p $DIR/graphs
	mv $DIR/*.pdf $DIR/graphs
}

run_experiment_variable
graph_full ${BENCH_TIME} 0 35 5
#graph_zoom ${BENCH_TIME} 5 30 1 1 0.1
cleanup
