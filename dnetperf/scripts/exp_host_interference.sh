#!/bin/bash

# Script for reproducing Figure 7 from the NOVA paper:
# host CPU interference and NOVA's adaptive load shifting.
#
#   7(a) host CPU, no interference               -> pkt_data_no_interf.csv
#   7(b) host CPU, interference, monitoring off   -> pkt_data_interf.csv
#   7(c) adaptive, interference, monitoring on    -> pkt_data_offload.csv
#
# Usage:
#   ./scripts/exp_host_interference.sh [options]
#
# Options:
#   --result-dir DIR   Output directory for CSVs/plots   (default: results/host_interference)

set -euo pipefail

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# Server (host) SSH
SERVER_SSH="node0"

# bess-nm checkout on the host (its $HOME does not contain bess-nm)
SERVER_BESS_NM="/proj/sandstorm-PG0/eurosys-ae/NOVA"

# bess-nm checkout on the DPU (relative to the DPU user's $HOME; kept
# unexpanded here so it's expanded remotely by the DPU's shell)
DPU_BESS_NM="~/NOVA"

# DPU ssh (reached through the host as a jump host)
DPU_SSH="ubuntu@192.168.100.2"
SSH_OPTS="-o StrictHostKeyChecking=no -J ${SERVER_SSH} ${DPU_SSH}"

# Server-side MAC address (for client -M/--server-mac). Update by hand.
SERVER_MAC="b8:3f:d2:54:8e:fe"

# Client lcores
LCORES="32,33,34,35"

# Client load: fixed 500 Kpps, 1 function, 30s run
BENCH_TIME=30
FIXED_PPS=500000
NFUNC=1

# CPU core the interference generator competes on.
# The server pipeline is started with the default ncpu=1, and
# naam_common.py pins worker 0 to core 0 (add_worker(0, 0)), so
# bessd's poll thread runs on core 0.
BESSD_CORE=0

# Interference timing (relative to client start)
INTERFERENCE_START=10
INTERFERENCE_DURATION=10

RAND_FOLDER="host_interference"
RESULT_DIR="${SCRIPT_DIR}/results/${RAND_FOLDER}"

usage() {
	grep '^#   ' "$0" | sed 's/^#   /  /'
	exit 0
}

while [[ $# -gt 0 ]]; do
	case $1 in
		--result-dir) RESULT_DIR=$2; shift 2 ;;
		-h|--help)    usage ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

check_client_bin() {
	local client_bin="${SCRIPT_DIR}/../client/build/client"
	if [ ! -x "${client_bin}" ]; then
		echo "[EXP] ERROR: Client binary not found: ${client_bin}" >&2
		exit 1
	fi
}

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

create_result_dir() {
	if [ ! -d "${RESULT_DIR}" ]; then
		mkdir -p "${RESULT_DIR}"
	fi
}

set_eswitch_host() {
	echo "[EXP] Setting e-switch to send traffic to host first"
	ssh ${SSH_OPTS} "sh -c 'cd ${DPU_BESS_NM}/scripts; ./switchctl.sh host'"
	echo ""
}

start_server() {
	echo "[EXP] Starting server (dma_read_write) on host"
	ssh ${SERVER_SSH} "sh -c 'cd ${SERVER_BESS_NM}/experiments; ./run_exp.py -c CLIENT_REG_MULTINODE/server_dma_rw_host.bess -b CLIENT_REG_MULTINODE/dma_read_write.c -j'"
	pin_server_core
	sleep 5
	echo ""
}

# run_exp.py starts bessd without pinning it to a specific CPU: bess.add_worker()
# only sets BESS's internal worker->core mapping, it does not set the OS
# thread's affinity, so bessd's poll thread ends up floating wherever the
# scheduler happens to place it. Pin it explicitly so it actually lands on
# BESSD_CORE, matching the core gen_interference() competes on.
# bessd runs as a supervisor/worker pair that both keep the "bessd" comm name
# (e.g. PID 331909 parent, 331910 child doing the actual polling); pgrep -x
# matches both, so take the highest PID (the child, deepest in the chain,
# the one actually busy-polling).
# Also: the busy-polling worker is its own thread (a different TID from the
# main "bessd" thread, e.g. shows up as "grpcpp_sync_ser" in ps -T), so
# pinning only the main TID leaves the real hot loop unpinned. Pin every
# thread under the process instead.
pin_server_core() {
	echo "[EXP] Pinning bessd to core ${BESSD_CORE}"
	ssh ${SERVER_SSH} "PID=\$(pgrep -x bessd | sort -n | tail -1); for t in /proc/\${PID}/task/*; do sudo taskset -pc ${BESSD_CORE} \$(basename \$t) > /dev/null; done; echo pinned all threads of \$PID" \
		|| echo "[EXP] WARNING: could not pin bessd to core ${BESSD_CORE}" >&2
}

stop_server() {
	echo "[EXP] Stopping server bessd"
	# May legitimately fail if bessd crashed/hung under interference; don't let
	# that abort the whole script (set -e) and skip remaining figures/cleanup.
	ssh ${SERVER_SSH} "sh -c 'cd ${SERVER_BESS_NM}/bessctl; ./bessctl daemon stop'" \
		|| echo "[EXP] WARNING: stop_server failed (bessd may have crashed); continuing." >&2
	echo ""
}

send_meminfo() {
	echo "[EXP] Sending memory info to DPU"
	ssh ${SERVER_SSH} "sh -c 'cd ${SERVER_BESS_NM}/scripts; ./send_meminfo.sh'"
	echo ""
}

start_server_dpu() {
	echo "[EXP] Starting server (dma_read_write) on DPU"
	ssh ${SSH_OPTS} "sh -c 'cd ${DPU_BESS_NM}; ./experiments/run_exp.py -e experiments/CLIENT_REG_MULTINODE/ -c experiments/CLIENT_REG_MULTINODE/server_dma_rw_dpu.bess -b experiments/CLIENT_REG_MULTINODE/dma_read_write.c -j'"
	sleep 5
	echo ""
}

stop_server_dpu() {
	echo "[EXP] Stopping DPU server bessd"
	ssh ${SSH_OPTS} "sh -c 'cd ${DPU_BESS_NM}/bessctl; ./bessctl daemon stop'" \
		|| echo "[EXP] WARNING: stop_server_dpu failed (bessd may have crashed); continuing." >&2
	echo ""
}

run_client() {
	# $1: output csv file name
	sudo ${SCRIPT_DIR}/../client/build/client -l ${LCORES} --iova-mode=va --socket-mem=2048 -- \
		-s 192.168.1.2 -M ${SERVER_MAC} -p 10002 -t ${BENCH_TIME} -T set -b 32 -n ${NFUNC} \
		-m fixed -r ${FIXED_PPS} --src-port-start 1024 --src-port-end 1031 \
		-o "${RESULT_DIR}/$1"
}

plot_results() {
	# $1: csv file name (same one passed to run_client)
	echo "[EXP] Plotting results from $1"
	python3 "${SCRIPT_DIR}/plot_host_interference.py" \
		-f "${RESULT_DIR}/$1" \
		-c "${SCRIPT_DIR}/../confs/plot_host_interf.conf"
	echo ""
}

gen_interference() {
	echo "[EXP] Generating ${INTERFERENCE_DURATION}s of interference on host core ${BESSD_CORE}"
	# timeout intentionally kills gen_interference.sh's infinite loop after
	# INTERFERENCE_DURATION seconds, so ssh exits 124 here on the expected path.
	#
	# Plain SCHED_OTHER only gets gen_interference.sh a ~50/50 CFS split with
	# bessd's poll thread, which bessd's capacity absorbs at this client's
	# offered rate without any queueing/latency impact. Run it SCHED_FIFO (via
	# chrt) so it actually preempts bessd's poll loop, like a real noisy
	# neighbor, instead of just time-sharing the core with it.
	ssh ${SERVER_SSH} "sh -c 'cd ${SERVER_BESS_NM}/scripts; timeout ${INTERFERENCE_DURATION} sudo chrt -f 99 taskset -c ${BESSD_CORE} ./gen_interference.sh'" || true
	echo ""
}

start_monitor_host() {
	echo "[EXP] Starting host interference monitor"
	# pkill must run in its own ssh invocation: if it shared a command line
	# with the nohup line below, pkill -f would match that command line too
	# (it contains "monitor_interference_host.py" unbracketed) and kill the
	# very shell that's supposed to launch the monitor.
	ssh ${SERVER_SSH} "pkill -f '[m]onitor_interference_host.py'" || true
	ssh ${SERVER_SSH} "sh -c 'cd ${SERVER_BESS_NM}/scripts; nohup ./monitor_interference_host.py > /tmp/monitor_interference_host.log 2>&1 &'" || true
	echo ""
}

stop_monitor_host() {
	echo "[EXP] Stopping host interference monitor"
	ssh ${SERVER_SSH} "pkill -f '[m]onitor_interference_host.py'" || true
}

start_monitor_dpu() {
	echo "[EXP] Starting DPU interference monitor"
	# See start_monitor_host for why pkill needs its own ssh invocation.
	ssh ${SSH_OPTS} "pkill -f '[m]onitor_interference_dpu.py'" || true
	ssh ${SSH_OPTS} "sh -c 'cd ${DPU_BESS_NM}/scripts; nohup ./monitor_interference_dpu.py > /tmp/monitor_interference_dpu.log 2>&1 &'" || true
	echo ""
}

stop_monitor_dpu() {
	echo "[EXP] Stopping DPU interference monitor"
	ssh ${SSH_OPTS} "pkill -f '[m]onitor_interference_dpu.py'" || true
}

# 7(a): host CPU, no interference
run_fig7a() {
	echo "[EXP] === Figure 7(a): host CPU, no interference ==="
	start_server
	if run_client "pkt_data_no_interf.csv"; then
		plot_results "pkt_data_no_interf.csv"
	else
		echo "[EXP] WARNING: client failed for fig7a; skipping plot." >&2
	fi
	stop_server
}

# 7(b): host CPU, interference, monitoring off
run_fig7b() {
	echo "[EXP] === Figure 7(b): host CPU, interference, monitoring off ==="
	start_server

	run_client "pkt_data_interf.csv" &
	client_pid=$!

	sleep ${INTERFERENCE_START}
	gen_interference

	if wait ${client_pid}; then
		plot_results "pkt_data_interf.csv"
	else
		echo "[EXP] WARNING: client failed for fig7b; skipping plot." >&2
	fi
	stop_server
}

# 7(c): adaptive, interference, monitoring on
run_fig7c() {
	echo "[EXP] === Figure 7(c): adaptive, interference, monitoring on ==="
	start_server
	send_meminfo
	start_server_dpu
	start_monitor_host
	start_monitor_dpu

	run_client "pkt_data_offload.csv" &
	client_pid=$!

	sleep ${INTERFERENCE_START}
	gen_interference

	if wait ${client_pid}; then
		plot_results "pkt_data_offload.csv"
	else
		echo "[EXP] WARNING: client failed for fig7c; skipping plot." >&2
	fi
	stop_monitor_host
	stop_monitor_dpu
	stop_server_dpu
	stop_server
}

check_client_bin
check_ssh
create_result_dir
set_eswitch_host

run_fig7a
run_fig7b
run_fig7c

echo "[EXP] Done! Results in ${RESULT_DIR}"
