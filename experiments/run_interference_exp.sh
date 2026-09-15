#!/bin/bash

set -euxo pipefail

echo "Starting bess daemon..."
./run_exp.py -c CLIENT_REG_MULTINODE/server_dma_rw_host.bess -b CLIENT_REG_MULTINODE/dma_read_write.c -j

../scripts/monitor_interference_host.py &
pid_monitor=$!

ssh ubuntu@192.168.100.2 "nohup ~/bess-nm/scripts/monitor_interference_dpu.py &> ~/bess-nm/monitor_interference_dpu.log < /dev/null &"

echo "Starting client..."
ssh 138.37.32.108 'nohup sudo ~/dnetperf/client/build/client -l 1,3,5,7 -- 192.168.1.2 10002 30 set 32 fixed 5000000 ~/dnetperf/interference_policy2.csv &> ~/dnetperf/client.log < /dev/null & echo $! > ~/dnetperf/client.pid'
client_pid=$(ssh 138.37.32.108 "cat ~/dnetperf/client.pid")

sleep 10
taskset -c 0 ../scripts/gen_interference.sh &
pid_interference=$!
sleep 10
kill -9 $pid_interference

# Wait unitl the client exits
ssh 138.37.32.108 "while sudo kill -0 $client_pid; do sleep 1; done" &> /dev/null
kill -9 $pid_monitor

#../bessctl/bessctl command module queue_inc0 get_stats EmptyArg
../bessctl/bessctl daemon stop
