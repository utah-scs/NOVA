#!/bin/bash

client_ip=$1
server_ip=$2
bench_time=$3
payload_type=$4
batch_size=$5
result_dir=$6

# Check number of arguments
if [ $# -ne 6 ]; then
		echo "Usage: $0 client_ip server_ip bench_time payload_type batch_size result_dir"
		echo ""
		echo "client_ip: IP address of the client"
		echo "server_ip: IP address of the server"
		echo "bench_time: Benchmark time in seconds"
		echo "payload_type: Payload type ('set', 'list')"
		echo "batch_size: Batch size"
		echo "result_dir: Directory to store the results"
		exit 1
fi

if [[ "$payload_type" == "set" ]]; then
	payload_size=339
elif [[ "$payload_type" == "list" ]]; then
	payload_size=316
fi

sudo ./client/build/client -l 2 --socket-mem=128 -- UDP_CLIENT $client_ip $server_ip 10001 10002 $bench_time $payload_type $payload_size $batch_size > ${result_dir}/client_${client_ip}
