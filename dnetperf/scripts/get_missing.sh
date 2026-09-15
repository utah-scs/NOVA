#!/usr/bin/env bash

## This script will run the client for 10 times
## and report the missing packets for each run

set -euo pipefail

SERVER_SSH_URL="ashfaq@mars"
DPU_SSH_URL="ssh ubunut@192.168.100.2"
RULE_TOGGLE_CMD="/home/ubuntu/bess-nm/switchctl.sh toggle"

NUM_RUNS=25
DEFAULT_POLICY="host"

# replace: Replace old rule
# priority: Priority based replacement
#DEFAULT_POLICY_INSTALL_TYPE="replace"
DEFAULT_POLICY_INSTALL_TYPE="replace"
RESULT_DIR="results"

if [ ! -d "$RESULT_DIR" ]; then
	mkdir "$RESULT_DIR"
fi

# Run dnetperf for N times
# Single threaded rx/tx
# args:
# $1. Expriment directory
# $2. Benchmark time
run_client_single() {
	for i in $(seq 1 ${NUM_RUNS}); do
		echo "Running $i"
		sudo ./client/build/client -l 2 --socket-mem=128 -- UDP_CLIENT 192.168.1.3 192.168.1.2 10001 10002 $2 set 357 1 &> $1/run_$i.txt
		sleep 1
	done
}

# Multi threaded rx/tx
run_client_multi() {
	for i in $(seq 1 ${NUM_RUNS}); do
		echo "Running $i"
		sudo ./client/build/client -l 0,1,2 --socket-mem=128 -- UDP_CLIENT 192.168.1.3 192.168.1.2 10001 10002 $1 set 357 4 &> results/run_$i.txt
		sleep 1
	done
}

# Print results
# args:
# $1. Experiment directory
# $2. Benchmark time
# $3. Benchmark type
print_results() {
	avg_sent=$(grep Missing $1/run_* | awk 'BEGIN{total=0}{total += $2}END{printf "%lf", total/NR}')
	avg_recv=$(grep Missing $1/run_* | awk 'BEGIN{total=0}{total += $4}END{printf "%lf", total/NR}')
	avg_miss=$(grep Missing $1/run_* | awk 'BEGIN{total=0}{total += $6}END{printf "%lf", total/NR}')
	avg_sent_mpps=$(awk -v avg=${avg_sent} -v t=${2} 'BEGIN {printf "%lf", avg/t/1000000}')
	avg_recv_mpps=$(awk -v avg=${avg_recv} -v t=${2} 'BEGIN {printf "%lf", avg/t/1000000}')
	avg_miss_mpps=$(awk -v avg=${avg_miss} -v t=${2} 'BEGIN {printf "%lf", avg/t/1000000}')
	stddev_sent=$(grep Missing $1/run_* | awk 'BEGIN{total=0;total_sq=0}{total += $2;total_sq += $2*$2}END{printf "%lf", sqrt(total_sq/NR - (total/NR)**2)}')
	stddev_recv=$(grep Missing $1/run_* | awk 'BEGIN{total=0;total_sq=0}{total += $4;total_sq += $4*$4}END{printf "%lf", sqrt(total_sq/NR - (total/NR)**2)}')
	stddev_miss=$(grep Missing $1/run_* | awk 'BEGIN{total=0;total_sq=0}{total += $6;total_sq += $6*$6}END{printf "%lf", sqrt(total_sq/NR - (total/NR)**2)}')
	stddev_sent_mpps=$(awk -v std=${stddev_sent} -v t=${2} 'BEGIN {printf "%lf", std/t/1000000}')
	stddev_recv_mpps=$(awk -v std=${stddev_recv} -v t=${2} 'BEGIN {printf "%lf", std/t/1000000}')
	stddev_miss_mpps=$(awk -v std=${stddev_miss} -v t=${2} 'BEGIN {printf "%lf", std/t/1000000}')
	echo "[$3] Average sent:  $avg_sent_mpps Mpps +-$stddev_sent_mpps"
	echo "[$3] Average recv:  $avg_recv_mpps Mpps +-$stddev_recv_mpps"
	echo "[$3] Average miss:  $avg_miss_mpps Mpps +-$stddev_miss_mpps"
}

# Set eswitch policy
# args:
# $1. policy
# $2. policy install type: replace, priority
set_eswitch_policy() {
	ssh -o StrictHostKeyChecking=no $SERVER_SSH_URL ssh ubuntu@192.168.100.2 /home/ubuntu/bess-nm/switchctl.sh $1 $2 &> /dev/null
}

# Set default policy
set_default_policy() {
	echo "Setting policy to $DEFAULT_POLICY"
	if [ $DEFAULT_POLICY == "host" ]; then
		set_eswitch_policy "host" $DEFAULT_POLICY_INSTALL_TYPE
	else
		set_eswitch_policy "dpu" $DEFAULT_POLICY_INSTALL_TYPE
	fi
}

# Run the client and change the eswitch policy
# between each run
# args:
# $1. Expriment directory
# $2. Benchmark time
# $3. Toggle time
# $4. Toggle type: replace, priority
run_client_bench_policy() {
	for i in $(seq 1 ${NUM_RUNS}); do
		set_default_policy
		echo "Running with policy $i"
		sudo ./client/build/client -l 2 --socket-mem=128 -- UDP_CLIENT 192.168.1.3 192.168.1.2 10001 10002 $2 set 357 1 &> $1/run_$i.txt &
		pid=$!
		sleep $3
		echo "Toggling policy"
		set_eswitch_policy "toggle" $4
		wait $pid
		sleep 1
	done
}

run_client_data_graph() {
	set_default_policy
	echo "Running with policy"
	sudo ./client/build/client -l 2 --socket-mem=128 -- 192.168.1.2 10002 30 set 357 1 packet_data_policy_4.csv &
	pid=$!
	sleep 15
	echo "Toggling policy"
	set_eswitch_policy "toggle" $DEFAULT_POLICY_INSTALL_TYPE
	wait $pid
}

usage() {
	echo "Usage: $0 Following commands are supported:"
	echo "		single  <bench_time>			- Run single threaded client for <bench_time> seconds"
	echo "		multi	<bench_time>			- Run multi threaded client for <bench_time> seconds"
	echo "		policy	<bench_time> <toggle_time>	- Run single threaded client for <bench_time> seconds and toggle eswitch policy after <toggle_time> seconds"
	echo "		bench	<bech_time>  <toggle_time>	- Run both single and policy benchmarks"
}

run_client_data_graph
exit

# Check number of arguments
if [ $# -lt 1 ]; then
	usage
 	exit 1
fi

if [ $1 = "single" ]; then
	# Check number of arguments
	if [ $# -lt 2 ]; then
		usage
		exit 1
	fi
	
	bench_dir="$RESULT_DIR/single_$(date +%s)"
	mkdir "$bench_dir"
	set_default_policy
	run_client_single $bench_dir $2
	print_results $bench_dir $2 "single"

elif [ $1 = "multi" ]; then
	# Check number of arguments
	if [ $# -lt 2 ]; then
		usage
		exit 1
	fi
	
	bench_dir="$RESULT_DIR/multi_$(date +%s)"
	mkdir "$bench_dir"
	set_default_policy
	run_client_multi $bench_dir $2
	print_results $bench_dir $2 "multi"

elif [ $1 = "policy" ]; then
	# Check number of arguments
	if [ $# -lt 3 ]; then
		usage
		exit 1
	fi
	
	bench_dir="$RESULT_DIR/policy_$(date +%s)"
	mkdir "$bench_dir"
	set_default_policy
	run_client_bench_policy $bench_dir $2 $3 $DEFAULT_POLICY_INSTALL_TYPE
	print_results $bench_dir $2 "policy"

elif [ $1 = "bench" ]; then
	# Check number of arguments
	if [ $# -lt 3 ]; then
		usage
		exit 1
	fi
	
	bench_dir_single="$RESULT_DIR/single_$(date +%s)"
	mkdir "$bench_dir_single"
	set_default_policy
	run_client_single $bench_dir_single $2

	sleep 1
	
	bench_dir_policy="$RESULT_DIR/policy_$(date +%s)"
	mkdir "$bench_dir_policy"
	set_default_policy
	run_client_bench_policy $bench_dir_policy $2 $3 $DEFAULT_POLICY_INSTALL_TYPE

	print_results $bench_dir_single $2 "single"
	print_results $bench_dir_policy $2 "policy"

else
	usage
	exit 1
fi


