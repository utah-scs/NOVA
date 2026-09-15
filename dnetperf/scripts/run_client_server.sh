#!/bin/bash

# Run all the clients from the server
# This script should be run from the
# server machine.

# Type of the payload
# Current supported payloads are:
# 	* set
# 	* list
function usage() {
  echo "usage: $(basename $0)"
	echo ""
	echo " 	-w, --workload WORKLOAD"
	echo " 		Workload for the experiment."
	echo " 		Currently supported workloads are list, set"
	echo ""
	echo " 	-t, --time TIME"
	echo " 		Time to run the benchmark"
	echo ""
	echo " 	-n, --clients NCLIENTS"
	echo " 		Total number of clients"
	echo ""
	echo " 	-e, --experiment-name NAME"
	echo " 		Name of the cloudlab experiment"
}

while [[ $# -gt 0 ]]; do
	case $1 in 
		-w|--workload)
			payload_type="$2"
			shift 2
			;;
		-n|--clients)
			total_clients="$2"
			shift 2
			;;
		-t|--time)
			bench_time="$2"
			shift 2
			;;
		-e|--experiment-name)
			exp_name="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		-*|--*)
      echo "Uknown option" 1>&2
      echo 1>&2
      usage 1>&2
      exit 1
      ;;
		*)
      echo "Uknown argument" 1>&2
      echo 1>&2
      usage 1>&2
      exit 1
  esac
done

if [[ $payload_type == "set" ]]; then
	bpf_file=bpf.c
elif [[ $payload_type == "list" ]]; then
	bpf_file=linked_list.c
else
	echo "Unsupported payload type: $payload_type"
	usage
	exit 1
fi

echo "Bechmark for payload: $payload_type"

# Bess directory
bess_dir=/proj/uic-dcs-PG0/ashfaq/naam/bess-nm

# Server IP in the dpdk interface
server_ip='10.10.1.1'

# Generate client URLs
clients_ssh=()
clients_ip=()
for i in $(seq 1 $total_clients); do

	# Client URLs for ssh
	clients_ssh+=("node${i}.${exp_name}.uic-dcs-pg0.utah.cloudlab.us")

	# Client IPs in the DPDK interface
	clients_ip+=("10.10.1.$((i + 1))")
done

configure_server() {
	pushd $bess_dir
	echo "Installing OFED driver..."
	./install_mlx_ofed.sh
	echo "Setting up bess..."
	./setup.sh
	pushd experiments
	echo "Running experiments..."
	./run_exp.py -e CLIENT_REG_MULTINODE/ -c server_simple.bess -f $bpf_file -j &> /dev/null
	popd
	popd
}

run_server_jit() {
	pushd ${bess_dir}/experiments
	echo "Running experiments with JIT on..."
	./run_exp.py -e CLIENT_REG_MULTINODE/ -c server_simple.bess -f $bpf_file -j
	popd
}

run_server_nojit() {
	pushd ${bess_dir}/experiments
	echo "Running experiments with JIT off..."
	./run_exp.py -e CLIENT_REG_MULTINODE/ -c server_simple.bess -f $bpf_file
	popd
}

check_client_ssh_connection() {
	echo "Checking ssh connection to clients..."

	failed=false
	for i in ${!clients_ssh[@]}; do
		echo -n "Checking connection to node$((i+1)): "
		ssh -o StrictHostKeyChecking=no ${clients_ssh[$i]} exit &> /dev/null

		if [[ "$?" != "0" ]]; then
			echo "FAILED"
			failed=true
		else
			echo "OK"
		fi
	done

	if [[ "$failed" == "true" ]]; then
		echo "SSH communication with some nodes failed"
		exit 1
	fi
}

configure_clients() {
	echo "Configuring clients..."
	for i in ${!clients_ssh[@]}; do
		ssh -o StrictHostKeyChecking=no ${clients_ssh[$i]} "pushd $(pwd) && ./install_dpdk.sh && popd"

		if [[ "$i" == "$((total_clients - 1))" ]]; then
			break
		fi
	done

	echo "DONE"
	echo ""
}

check_client_configuration() {
	result_dir=$(pwd)/results/tests
	[ ! -e $result_dir ] && mkdir -p $result_dir
	
	echo "Checking clinet configuration..."
	failed=false
	
	for i in ${!clients_ssh[@]}; do
		echo -n "Client @ ${clients_ip[$i]}: "

		ssh -o StrictHostKeyChecking=no ${clients_ssh[$i]} "pushd $(pwd) && ./run_client.sh ${clients_ip[$i]} $server_ip 1 $payload_type 32 $result_dir && popd" &> /dev/null

		if grep -q mean ${result_dir}/client_${clients_ip[$i]}; then
			echo "OK"
		else
			echo "FAILED"
			failed=true
		fi
	done
	
	if [[ "$failed" == "true" ]]; then
		echo "Some clients aren't configured properly"
		exit 1
	fi

	rm -r $result_dir
}

run_experiment() {
	result_dir=$(pwd)/results
	[ ! -e $result_dir ] && mkdir $result_dir

	echo "Benchmark started..."

	# Run 1 to 15 clients with batch size 1
	num_clients=1
	batch_size=1

	result_dir=${result_dir}/raw_data_$(date +%s)
	mkdir -p ${result_dir}/batch_${batch_size}

	while true; do
		dir_name=${result_dir}/batch_${batch_size}/client_${num_clients}
		mkdir $dir_name
		echo "Clients: ${num_clients} Dir: ${dir_name}"
		
		for i in ${!clients_ssh[@]}; do
			echo "Runnin client on ${clients_ip[$i]}"
			ssh -o StrictHostKeyChecking=no ${clients_ssh[$i]} "pushd /proj/uic-dcs-PG0/ashfaq/repos/dpdk_netperf/ && ./run_client.sh ${clients_ip[$i]} $server_ip $bench_time $payload_type $batch_size $dir_name && popd" &
			pids[$i]=$!

			if [[ "$i" == "$((num_clients - 1))" ]]; then
				break
			fi
		done &> /dev/null

		for pid in ${pids[*]}; do
			wait $pid
		done

		if [[ "$num_clients" == "$total_clients" ]]; then
			break
		fi
		
		num_clients=$((num_clients + 1))
		
		sleep 5
	done

	# Run all 15 clients together varying the batch size
	max_batch_size=64
	batch_size=2
	num_clients=$total_clients

	while true; do
		mkdir ${result_dir}/batch_${batch_size}
		dir_name=${result_dir}/batch_${batch_size}/client_${num_clients}
		mkdir $dir_name
		echo "Clients: ${num_clients} Dir: ${dir_name}"
		
		for i in ${!clients_ssh[@]}; do
			echo "Runnin client on ${clients_ip[$i]}"
			ssh -o StrictHostKeyChecking=no ${clients_ssh[$i]} "pushd /proj/uic-dcs-PG0/ashfaq/repos/dpdk_netperf/ && ./run_client.sh ${clients_ip[$i]} $server_ip $bench_time $payload_type $batch_size $dir_name && popd" &
			pids[$i]=$!

			if [[ "$i" == "$((num_clients - 1))" ]]; then
				break
			fi
		done &> /dev/null
		
		for pid in ${pids[*]}; do
			wait $pid
		done

		if [[ "$batch_size" == "$max_batch_size" ]]; then
			break
		fi

		batch_size=$((batch_size * 2))

		sleep 5
	done

	echo "Benchmark done!"
}

configure_server
sleep 5
run_server_jit
sleep 5
check_client_ssh_connection
configure_clients
sleep 5
check_client_configuration
run_experiment
sleep 5
run_server_nojit
sleep 5
run_experiment
