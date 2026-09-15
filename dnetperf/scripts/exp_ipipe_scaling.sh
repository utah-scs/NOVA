#!/bin/bash

# Script for collecting latency and throughput
# data from the server

set -euo pipefail

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# Directory the script was invoked from (used to resolve a relative
# -o/--output-dir against the caller's cwd, not against SCRIPT_DIR).
INVOKE_DIR="$(pwd)"


# Server SSH
SERVER_SSH="node0"

# Server-side MAC address (for client -M/--server-mac). Update by hand.
SERVER_MAC="b8:3f:d2:54:8e:fe"

# DPU ssh
DPU_SSH="ubuntu@192.168.100.2"

# bess-nm directory on the DPU
NOVA_DIR_DPU="~/NOVA"

# SSH options
#SSH_OPTS="${SERVER_SSH}"
SSH_OPTS="-o StrictHostKeyChecking=no -J ${SERVER_SSH} ${DPU_SSH}"

# lcores
LCORES="32,33,34,35"

PPS_START=500000
PPS_END=2000000
PPS_STEP=100000
BENCH_TIME=30
N_ITER=5

# Output directory for results. May be given as relative (resolved
# against the caller's cwd) or absolute via -o/--output-dir; either way
# it is resolved to an absolute path in OUTPUT_DIR below.
RAND_FOLDER="tenant_scaling"

usage() {
	echo "Usage: $0 [-o|--output-dir DIR]"
	echo ""
	echo "  -o, --output-dir DIR   Output directory for results (default: ${RAND_FOLDER})"
	echo "  -h, --help             Show this help message"
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		-o|--output-dir)
			RAND_FOLDER="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "[EXP] ERROR: Unknown argument: $1" >&2
			usage
			exit 1
			;;
	esac
done

# Resolve the output directory to an absolute path, rather than nesting
# it under ${SCRIPT_DIR}/results/. A relative RAND_FOLDER is resolved
# against the directory the script was invoked from.
if [[ "${RAND_FOLDER}" = /* ]]; then
	OUTPUT_DIR="${RAND_FOLDER}"
else
	OUTPUT_DIR="${INVOKE_DIR}/${RAND_FOLDER}"
fi

check_ssh() {
	echo "[EXP] Checking SSH access to ${SERVER_SSH} and ${DPU_SSH}"
	if ! ssh -o BatchMode=yes -o ConnectTimeout=5 ${SERVER_SSH} "echo -n" &> /dev/null; then
		echo "[EXP] ERROR: Cannot SSH to server (${SERVER_SSH}). Check connectivity/keys and try again." >&2
		exit 1
	fi
	if ! ssh -o BatchMode=yes -o ConnectTimeout=5 ${SSH_OPTS} "echo -n" &> /dev/null; then
		echo "[EXP] ERROR: Cannot SSH to DPU (${DPU_SSH} via ${SERVER_SSH}). Check connectivity/keys and try again." >&2
		exit 1
	fi
	echo "[EXP] SSH access OK"
	echo ""
}

check_dpu_dir() {
	echo "[EXP] Checking that ${NOVA_DIR_DPU} exists on DPU"
	if ! ssh ${SSH_OPTS} "sh -c 'test -d ${NOVA_DIR_DPU}'" &> /dev/null; then
		echo "[EXP] ERROR: Directory ${NOVA_DIR_DPU} does not exist on DPU (${DPU_SSH}). Check NOVA_DIR_DPU and try again." >&2
		exit 1
	fi
	echo "[EXP] DPU directory OK"
	echo ""
}

checkout_naam() {
	echo "[EXP] Checking out naam"
	echo ""
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/; git checkout main'" &> /dev/null
}

checkout_ipipe() {
	echo "[EXP] Checking out ipipe"
	echo ""
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/; git checkout ipipe'" &> /dev/null

}

build_bess() {
	echo "[EXP] Building BESS"
	echo ""
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/; ./scripts/setup.sh build_bess'"
	echo ""
}

start_naam() {
	echo "[EXP] Starting naam server"
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/experiments; bash start_naam.sh $1 $2 $3 $4'"
	echo ""
}

stop_naam() {
	echo "[EXP] Stopping naam server"
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/experiments; bash stop_naam.sh'"
	echo ""
}

start_ipipe() {
	echo "[EXP] Starting ipipe server"
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/experiments; bash start_ipipe.sh $1 $2 $3 $4'"
	echo ""
}

stop_ipipe() {
	echo "[EXP] Stopping ipipe server"
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/experiments; bash stop_ipipe.sh $1'"
	echo ""
}

set_eswitch_dpu() {
	echo "[EXP] Setting e-switch to send traffic to DPU"
	ssh ${SSH_OPTS} "sh -c 'cd ${NOVA_DIR_DPU}/scripts; ./switchctl.sh dpu'"
}

run_exp_lat_tput_naam() {
	for i in $(seq $PPS_START $PPS_STEP $PPS_END); do
		echo "[EXP] Running naam experiment with $i pps and $1 functions in ${RESULT_DIR}"

		sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova-mode=va --socket-mem=2048 -- -s 192.168.1.2 -M ${SERVER_MAC} -p 10002 -t ${BENCH_TIME} -T set -b 32 -n $1 -m fixed -r $i --src-port-start 1024 --src-port-end 1031 &> ${RESULT_DIR}/lat_tpu_${i}.txt
		sleep 5
	done
}

run_exp_lat_tput_ipipe() {
	for i in $(seq $PPS_START $PPS_STEP $PPS_END); do
		echo "[EXP] Running ipipe experiment with $i pps and $1 functions in ${RESULT_DIR}"

		sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova-mode=va --socket-mem=2048 -- -s 192.168.1.2 -M ${SERVER_MAC} -p 10002 -t ${BENCH_TIME} -T set -b 32 -n 1 -m fixed -r $i --src-port-start 1024 --src-port-end 1031 &> ${RESULT_DIR}/lat_tpu_${i}.txt
		sleep 5
	done
}

run_exp_func_scale_naam() {
	for i in $(seq ${N_ITER}); do
		echo "[EXP] Running naam experiment with ${PPS_START} pps and $1 functions in ${RESULT_DIR}"

		sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova-mode=va --socket-mem=2048 -- -s 192.168.1.2 -M ${SERVER_MAC} -p 10002 -t ${BENCH_TIME} -T set -b 32 -n $1 -m fixed -r ${PPS_START} --src-port-start 1024 --src-port-end 1031 &> ${RESULT_DIR}/lat_tpu_${i}.txt
		sleep 5
	done

}

run_exp_func_scale_ipipe() {
	for i in $(seq ${N_ITER}); do
		echo "[EXP] Running ipipe experiment with ${PPS_START} pps and $1 functions in ${RESULT_DIR}"

		sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova-mode=va --socket-mem=2048 -- -s 192.168.1.2 -M ${SERVER_MAC} -p 10002 -t ${BENCH_TIME} -T set -b 32 -n 1 -m fixed -r ${PPS_START} --src-port-start 1024 --src-port-end 1031 &> ${RESULT_DIR}/lat_tpu_${i}.txt
		sleep 5
	done
}

process_results_func() {
	echo "[EXP] Writing results to ${RESULT_FILE}"
	echo "tput_sent,tput_recv,lat_avg,lat_50th,lat_99th" > ${RESULT_FILE}
	for f in $(basename -a $(ls ${RESULT_DIR}/*.txt) | sort -n -t _ -k 3); do
		tput_sent=$(cat ${RESULT_DIR}/${f} | grep "^Sent:.*Mpps" | awk '{print $2}')
		tput_recv=$(cat ${RESULT_DIR}/${f} | grep "^Sent:.*Mpps" | awk '{print $5}')
		lat_avg=$(cat ${RESULT_DIR}/${f} | grep "mean" | awk '{print $4}')
		lat_50th=$(cat ${RESULT_DIR}/${f} | grep "median" | awk '{print $4}')
		lat_99th=$(cat ${RESULT_DIR}/${f} | grep "99th" | awk '{print $4}')
		echo "${tput_sent},${tput_recv},${lat_avg},${lat_50th},${lat_99th}" >> ${RESULT_FILE}
	done
}

process_results() {
	RESULT_DIR="${OUTPUT_DIR}/${1}/"
	RESULT_FILE="${RESULT_DIR}/func_results.csv"
	echo "[EXP] Writing results to ${RESULT_FILE}"
	echo "functions,tput_sent,tput_recv,lat_99th" > ${RESULT_FILE}
	for dir in $(basename -a $(ls -d ${RESULT_DIR}/func*/) | sort -n -t _ -k 2); do
		nfunc=$(echo $dir | awk -F _ '{print $2}')
		cat ${RESULT_DIR}/${dir}/lat_tpu_results.csv | tail -n +2 | awk -F , -v f=$nfunc 'BEGIN {t_s=0; t_r=0; lat=0} {t_s+=$1; t_r+=$2; lat+=$5} END{printf "%d,%f,%f,%f\n",f,t_s/NR,t_r/NR,lat/NR}' >> ${RESULT_FILE}
	done

}

create_result_dir() {
	RESULT_DIR="${OUTPUT_DIR}/${1}/func_${2}"
	RESULT_FILE="${RESULT_DIR}/lat_tpu_results.csv"

	# Check if the result directory exists
	# If it doesn't, create it
	if [ ! -d "$RESULT_DIR" ]; then
		mkdir -p "$RESULT_DIR"
	fi
}

generate_plots_lat_tput() {
	echo "[EXP] Generating plots"
	${SCRIPT_DIR}/plot_lat_tput.py ${RESULT_FILE} $1 $2 &> /dev/null
	echo "[EXP] Done! Result dir: ${RESULT_DIR}"
	echo ""
}

generate_plot_func_scale() {
	echo "[EXP] Generating function scaling plot"
	RESULT_DIR="${OUTPUT_DIR}/"
	RESULT_FILE_NAAM="${RESULT_DIR}/naam/func_results.csv"
	RESULT_FILE_IPIPE="${RESULT_DIR}/ipipe/func_results.csv"
	${SCRIPT_DIR}/plot_function_scale.py ${RESULT_FILE_NAAM} ${RESULT_FILE_IPIPE} &> /dev/null
	echo "[EXP] Done! Result dir: ${RESULT_DIR}"
	echo ""
}

# $1: number of cpu to use 
# $2: number of functions
run_naam_func() {
	echo "[EXP] Starting naam experiments with $2 functions (ignore if failed to stop server first time)"
	create_result_dir "naam" $2
	start_naam $1 $2 2048 65536 
	run_exp_func_scale_naam $2
	stop_naam
	process_results_func
}

# $1: number of maximum cpu to use
# $2: number of functions
run_ipipe_func() {
	create_result_dir "ipipe" $2
	start_ipipe $1 $2 2048 65536 
	run_exp_func_scale_ipipe $2
	stop_ipipe $2
	process_results_func
}

run_naam() {
	echo "[EXP] Starting naam experiments"
	checkout_naam
	stop_naam
	checkout_ipipe
	stop_ipipe 16
	checkout_naam
	build_bess
	set_eswitch_dpu

	run_naam_func 4 1 # use 4 cores for 1 function
	run_naam_func 4 2
	run_naam_func 4 4
	run_naam_func 4 8
	run_naam_func 4 16
	run_naam_func 4 32
	run_naam_func 4 64
	run_naam_func 4 128

	process_results "naam"
}

run_ipipe() {
	echo "[EXP] Starting ipipe experiments"
	checkout_naam
	stop_naam
	checkout_ipipe
	stop_ipipe 16
	checkout_ipipe
	build_bess
	set_eswitch_dpu

	run_ipipe_func 4 1 # use 4 cores for 1 function
	run_ipipe_func 4 2
	run_ipipe_func 4 4
	run_ipipe_func 4 8

	process_results "ipipe"
}

# Compare NAAM with small and large number
# of pre-allocated packet buffers
# 262144 vs 65536
compare_naam() {
	create_result_dir "large" 8
	checkout_naam
	build_bess
	start_naam 8 1024 262144 
	run_experiments_naam 8
	stop_naam
	process_results_func
	generate_plots 1 100
	
	create_result_dir "small" 8
	start_naam 8 256 65536
	run_experiments_naam 8
	stop_naam
	process_results_func
	generate_plots 1 100
}

# Compare NAAM with iPipe
compare_ipipe() {
	#run_naam
	#run_ipipe
	generate_plot_func_scale
}

check_ssh
check_dpu_dir

#compare_naam
compare_ipipe
#generate_plot_func_scale
