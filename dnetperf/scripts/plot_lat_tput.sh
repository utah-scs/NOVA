#!/bin/bash

# Script for collecting latency and throughput
# data from the server

set -xeuo pipefail

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

RESULT_DIR="${SCRIPT_DIR}/results/lat_tpu/$(date +%Y%m%d%H%M%S)"
RESULT_FILE="${RESULT_DIR}/lat_tpu_results.csv"

# Check if the result directory exists
# If it doesn't, create it
if [ ! -d "$RESULT_DIR" ]; then
	mkdir -p "$RESULT_DIR"
fi

# Server SSH
SERVER_SSH=""

# lcores
LCORES="0,2,4,6"

PPS_START=100000
PPS_END=10000000
PPS_STEP=100000
BENCH_TIME=30

run_experiments_host() {
	for i in $(seq $PPS_START $PPS_STEP $PPS_END); do
		echo "Running experiment with $i pps in ${RESULT_DIR}"

		# Start server
		#ssh ${SERVER_SSH} "pushd /home/ashfaq/bess-nm/experiments && ./run_exp.py -e MICA_MULTI -c server_simple_host_sep.bess -f bpf-get.c -f bpf-set.c -j && popd" &> /dev/null

		sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova=va --socket-mem=2048 -- 192.168.1.2 10002 ${BENCH_TIME} ht 32 fixed $i &> ${RESULT_DIR}/lat_tpu_${i}.txt
		sleep 5

		# Stop server
		#ssh ${SERVER_SSH} "pushd /home/ashfaq/bess-nm/ && ./bessctl/bessctl daemon stop && popd" &> /dev/null
	done
}

start_server() {
	# Start server
	ssh ${SERVER_SSH} "sh -c 'cd ~/bess-nm/; ./start_experiment.sh'" &> /dev/null
}

stop_server() {
	# stop server
	ssh ${SERVER_SSH} "sh -c 'cd ~/bess-nm/; ./stop_experiment.sh'" &> /dev/null
}

start_monitor_script() {
	# start DPU pmd port monitor script
	ssh -o StrictHostKeyChecking=no -J ${SERVER_SSH} ubuntu@192.168.100.2 "sh -c 'cd ~/bess-nm; nohup ./monitor_port.py -y -c 7 > /dev/null 2>&1 &'"
}

run_experiments_host_dpu() {
	for i in $(seq $PPS_START $PPS_STEP $PPS_END); do
		echo "Running experiment with $i pps in ${RESULT_DIR}"

		start_monitor_script
		sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova=va --socket-mem=2048 -- 192.168.1.2 10002 ${BENCH_TIME} ht 32 fixed $i &> ${RESULT_DIR}/lat_tpu_${i}.txt
		sleep 5
	done
}


process_results() {
	echo "Writing results to ${RESULT_FILE}"
	echo "tput_sent,tput_recv,lat_avg,lat_50th,lat_99th" > ${RESULT_FILE}
	for f in $(basename -a $(ls ${RESULT_DIR}/*.txt) | sort -n -t _ -k 3); do
		tput_sent=$(cat ${RESULT_DIR}/${f} | grep "Mpps" | awk '{print $2}')
		tput_recv=$(cat ${RESULT_DIR}/${f} | grep "Mpps" | awk '{print $5}')
		lat_avg=$(cat ${RESULT_DIR}/${f} | grep "mean" | awk '{print $4}')
		lat_50th=$(cat ${RESULT_DIR}/${f} | grep "median" | awk '{print $4}')
		lat_99th=$(cat ${RESULT_DIR}/${f} | grep "99th" | awk '{print $4}')
		echo "${tput_sent},${tput_recv},${lat_avg},${lat_50th},${lat_99th}" >> ${RESULT_FILE}
	done
}

#start_server
#run_experiments_host_dpu
run_experiments_host
#stop_server
process_results
${SCRIPT_DIR}/plot_lat_tput.py ${RESULT_FILE}
echo "Done! Result dir: ${RESULT_DIR}"
