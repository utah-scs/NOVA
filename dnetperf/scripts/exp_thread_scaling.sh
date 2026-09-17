#!/usr/bin/env bash
#
# Thread-scaling throughput experiment for the MICA_MULTI server.
#
# For each thread count (1..MAX_THREADS):
#   1. Start the server on node-0 via SSH
#   2. Push meminfo to the DPU
#   3. Start the DPU port monitor
#   4. Sweep offered load until miss% >= MAX_MISS_PCT (binary-search not needed;
#      a linear sweep with the configured step is sufficient and reproducible)
#   5. Append every data point to a CSV file
#
# Usage:
#   ./scripts/exp_thread_scaling.sh [options]
#
# Options:
#   --max-threads N      Highest thread count to test          (default: 8)
#   --max-miss-pct N     Stop sweep when miss% >= N            (default: 5)
#   --pps-start N        First offered load in pps             (default: 500000)
#   --pps-step N         Load increment per step               (default: 500000)
#   --pps-max N          Hard cap on offered load              (default: 10000000)
#   --bench-time N       Client run duration per step (s)      (default: 10)
#   --cooldown N         Seconds to wait between thread iters  (default: 5)
#   --server-wait N      Seconds to wait after server start    (default: 10)
#   --result-dir DIR     Output directory                      (default: results/thread_scaling_<timestamp>)
#   --result-file FILE   Append results to this specific CSV file (skips header if file exists)
#   --num-keys N         Key population size passed to client  (default: client compiled-in default)
#   --workload W         YCSB workload(s), comma-separated: A (50r/50w),
#                        B (95r/5w), C (100r)                 (default: B)
#   --key-dist D         Key distribution(s), comma-separated: uniform,zipf
#                                                              (default: uniform)
#   --zipf-theta T       Zipf skew exponent in (0,1)           (default: 0.99, only with --key-dist zipf)
#   --min-threads N      Lowest thread count to test                (default: 1)
#   --warmup N           Warmup window in seconds; samples discarded (default: 2)
#   --dpu-only           Disable port-monitor start/stop/collect (default: off)

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
DPU_ONLY=false
MIN_THREADS=1
MAX_THREADS=8
MAX_MISS_PCT=5
PPS_START=500000
PPS_STEP=500000
PPS_MAX=10000000
BENCH_TIME=10
COOLDOWN=2
SERVER_WAIT=2
NUM_KEYS=""          # empty = use client's compiled-in default
YCSB_WORKLOAD="B"
KEY_DIST="uniform"
ZIPF_THETA="0.99"
WARMUP=2

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR=""          # filled after arg parsing
RESULT_FILE_OVERRIDE=""  # if set, append to this file instead of creating a new one

# Fixed client / server parameters
SERVER_SSH_USER="${SUDO_USER:-${USER}}"
SERVER_SSH="${SERVER_SSH_USER}@node0"
SERVER_IP="10.10.1.1"
SERVER_MAC="b8:3f:d2:54:8e:fe"
SERVER_PORT="10002"
CLIENT_LCORES="32,33,34,35,36,37,38,39,40,41"
CLIENT_SOCKET_MEM="128"
PAYLOAD_TYPE="ht"
BATCH_SIZE="32"
NUM_CLIENT_QUEUES="1"
RETRY_TIMEOUT="50000"
MAX_RETRIES="3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLIENT_BIN="${SCRIPT_DIR}/../client/build/client"

BESS_NM_ROOT="/proj/sandstorm-PG0/eurosys-ae/NOVA"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
usage() {
    grep '^#   ' "$0" | sed 's/^#   /  /'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --min-threads)  MIN_THREADS=$2;  shift 2 ;;
        --max-threads)  MAX_THREADS=$2;  shift 2 ;;
        --max-miss-pct) MAX_MISS_PCT=$2; shift 2 ;;
        --pps-start)    PPS_START=$2;    shift 2 ;;
        --pps-step)     PPS_STEP=$2;     shift 2 ;;
        --pps-max)      PPS_MAX=$2;      shift 2 ;;
        --bench-time)   BENCH_TIME=$2;   shift 2 ;;
        --cooldown)     COOLDOWN=$2;     shift 2 ;;
        --server-wait)  SERVER_WAIT=$2;  shift 2 ;;
        --result-dir)   RESULT_DIR=$2;        shift 2 ;;
        --result-file)  RESULT_FILE_OVERRIDE=$2; shift 2 ;;
        --num-keys)     NUM_KEYS=$2;          shift 2 ;;
        --workload)     YCSB_WORKLOAD=$2;     shift 2 ;;
        --key-dist)     KEY_DIST=$2;          shift 2 ;;
        --zipf-theta)   ZIPF_THETA=$2;        shift 2 ;;
        --warmup)       WARMUP=$2;            shift 2 ;;
        --dpu-only)     DPU_ONLY=true;        shift ;;
        -h|--help)      usage ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -n "${RESULT_FILE_OVERRIDE}" ]]; then
    RESULT_FILE="${RESULT_FILE_OVERRIDE}"
    [[ -z "${RESULT_DIR}" ]] && RESULT_DIR="$(dirname "${RESULT_FILE}")"
else
    [[ -z "${RESULT_DIR}" ]] && RESULT_DIR="${SCRIPT_DIR}/results/thread_scaling_${TIMESTAMP}"
    RESULT_FILE="${RESULT_DIR}/results.csv"
fi

# --workload and --key-dist accept comma-separated lists; every combination
# of workload x key-dist is run in turn. YCSB_WORKLOAD/KEY_DIST are then
# reused as the "current" single value while iterating (see main()).
IFS=',' read -ra WORKLOADS <<< "${YCSB_WORKLOAD}"
IFS=',' read -ra KEY_DISTS <<< "${KEY_DIST}"

# LOG_SUBDIR (per-workload subfolder: ycsb{A,B,C}_{uniform,zipf}) is computed
# per combination inside main(), once YCSB_WORKLOAD/KEY_DIST are set to the
# current values in the sweep.
LOG_SUBDIR=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log() { echo "[$(date '+%H:%M:%S')] $*"; }

die() { echo "ERROR: $*" >&2; exit 1; }

# Run an SSH command silently; log success/failure with a label.
# Usage: ssh_quiet <label> <ssh_host> <remote_cmd>
ssh_quiet() {
    local label=$1 host=$2; shift 2
    local tmpfile
    tmpfile=$(mktemp)
    if ssh -o StrictHostKeyChecking=no "${host}" "$@" >"${tmpfile}" 2>&1; then
        log "  [OK] ${label}"
    else
        local rc=$?
        log "  [FAILED] ${label} (exit ${rc})"
        sed 's/^/    /' "${tmpfile}" >&2
        rm -f "${tmpfile}"
        return "${rc}"
    fi
    rm -f "${tmpfile}"
}

# Start the MICA_MULTI server with the given thread count.
# run_exp.py calls exitfn() first, so any previously running instance is
# stopped automatically before the new one starts.
start_server() {
    local nthreads=$1
    local num_keys_arg=""
    [[ -n "${NUM_KEYS}" ]] && num_keys_arg="--num-keys ${NUM_KEYS}"
    log "Starting host server (${nthreads} thread(s))..."
    ssh_quiet "host server started" "${SERVER_SSH}" \
        "cd '${BESS_NM_ROOT}' && experiments/run_exp.py \
        -e MICA_MULTI \
        -c experiments/MICA_MULTI/server_simple_host.bess \
        -b experiments/MICA_MULTI/mica-naam.c \
        -j -n ${nthreads} ${num_keys_arg}"
    log "Waiting ${SERVER_WAIT}s for readiness..."
    sleep "${SERVER_WAIT}"
}

# Push the DPDK meminfo files that the host bessd exported to the DPU.
copy_meminfo() {
    log "Pushing meminfo to DPU..."
    ssh -o StrictHostKeyChecking=no "${SERVER_SSH}" "${BESS_NM_ROOT}/scripts/send_meminfo.sh"
}

# Start the DPU-side port-monitor in the background (nohup inside the script).
start_monitor() {
    log "Starting DPU port monitor..."
    ssh -o StrictHostKeyChecking=no "${SERVER_SSH}" "${BESS_NM_ROOT}/scripts/start_monitor_port.sh"
}

# Start the MICA experiment on the DPU.
start_dpu_experiment() {
    local num_keys_arg=""
    [[ -n "${NUM_KEYS}" ]] && num_keys_arg="--num-keys ${NUM_KEYS}"
    log "Starting DPU experiment..."
    ssh_quiet "DPU experiment started" "${SERVER_SSH}" \
        "${BESS_NM_ROOT}/scripts/start_mica_dpu.sh ${num_keys_arg}"
    sleep "${SERVER_WAIT}"
}

# Stop the DPU-side port-monitor.
stop_monitor() {
    log "Stopping DPU port monitor..."
    ssh -o StrictHostKeyChecking=no "${SERVER_SSH}" "${BESS_NM_ROOT}/scripts/stop_monitor_port.sh"
}

# Stop NAAM server on host and DPU
stop_server() {
    log "Stopping NAAM server in DPU and host..."
    ssh -o StrictHostKeyChecking=no "${SERVER_SSH}" "${BESS_NM_ROOT}/scripts/stop_experiment.sh"
}

# Copy bessd.INFO from host and DPU after each pps step.
collect_logs() {
    local nthreads=$1 pps=$2
    local prefix="${LOG_SUBDIR}/t${nthreads}_pps${pps}"

    scp -o StrictHostKeyChecking=no \
        "${SERVER_SSH}:/tmp/bessd.INFO" \
        "${prefix}_host_bessd.log" >/dev/null 2>&1 \
        && log "    [OK] host bessd log saved" \
        || log "    [WARN] host bessd log not found"

    ssh -o StrictHostKeyChecking=no "${SERVER_SSH}" \
        "ssh ubuntu@192.168.100.2 'cat /tmp/bessd.INFO'" \
        > "${prefix}_dpu_bessd.log" 2>/dev/null \
        && log "    [OK] DPU bessd log saved" \
        || log "    [WARN] DPU bessd log not found"
}

# Copy monitor_port.log from the DPU after each thread-count iteration.
collect_monitor_log() {
    local nthreads=$1

    ssh -o StrictHostKeyChecking=no "${SERVER_SSH}" \
        "ssh ubuntu@192.168.100.2 'cat /tmp/monitor_port.log'" \
        > "${LOG_SUBDIR}/t${nthreads}_monitor_port.log" 2>/dev/null \
        && log "  [OK] monitor port log saved" \
        || log "  [WARN] monitor port log not found"
}

# Run the client at the given PPS; capture all output to a file.
run_client() {
    local pps=$1
    local outfile=$2
    local extra_args=()
    [[ -n "${NUM_KEYS}" ]]       && extra_args+=(--num-keys "${NUM_KEYS}")
    extra_args+=(--workload "${YCSB_WORKLOAD}")
    extra_args+=(--key-dist "${KEY_DIST}")
    [[ "${KEY_DIST}" == "zipf" ]] && extra_args+=(--zipf-theta "${ZIPF_THETA}")
    local cmd=(sudo "${CLIENT_BIN}"
        -l "${CLIENT_LCORES}" --socket-mem="${CLIENT_SOCKET_MEM}" --
        -s "${SERVER_IP}" -M "${SERVER_MAC}" -p "${SERVER_PORT}"
        -t "${BENCH_TIME}" -T "${PAYLOAD_TYPE}" -b "${BATCH_SIZE}"
        -n "${NUM_CLIENT_QUEUES}" -m fixed -r "${pps}"
        --retry-timeout "${RETRY_TIMEOUT}" --max-retries "${MAX_RETRIES}"
        --warmup "${WARMUP}"
        "${extra_args[@]}")
    printf '$ %s\n\n' "${cmd[*]}" > "${outfile}"
    "${cmd[@]}" >> "${outfile}" 2>&1
}

# Parse the "Sent: X Mpps\tReceived: Y Mpps\tMissing: Z Mpps" line.
# $1 = output file, $2 = field index (2=sent, 5=recv, 8=missing)
parse_mpps() {
    local file=$1 field=$2
    grep -m1 "^Sent:.*Mpps.*Received:.*Mpps" "${file}" \
        | awk -v f="${field}" '{print $f}'
}

# Parse a latency line: "mean|median|99th latency (us): VALUE"
parse_latency() {
    local file=$1 keyword=$2
    grep -m1 "${keyword} latency" "${file}" | awk '{print $NF}'
}

# Parse the retry counters.
parse_retries() {
    grep -m1 "^Retries:" "$1" | awk '{print $2}'
}

parse_retries_exhausted() {
    grep -m1 "^Retry exhausted" "$1" | awk -F: '{gsub(/[[:space:]]/,"",$NF); print $NF}'
}

# Parse the "Host: X Mpps\tDPU: Y Mpps" line.
# $1 = output file, $2 = "Host" or "DPU"
parse_server_mpps() {
    local file=$1 label=$2
    grep -m1 "^Host:.*Mpps.*DPU:.*Mpps" "${file}" \
        | awk -v l="${label}" '{
            for (i=1; i<=NF; i++)
                if ($i == l":") { print $(i+1); exit }
        }'
}

# Compute miss% from Mpps values (floating-point via awk).
miss_pct() {
    local sent=$1 recv=$2
    awk -v s="${sent}" -v r="${recv}" \
        'BEGIN { printf "%.2f", (s > 0) ? (s - r) / s * 100 : 0 }'
}

# Return 1 (true in bash test) if $1 >= $2 (both floats).
float_ge() {
    awk -v a="$1" -v b="$2" 'BEGIN { exit (a >= b) ? 0 : 1 }'
}

# ---------------------------------------------------------------------------
# Load sweep for a given thread count
# ---------------------------------------------------------------------------
sweep_load() {
    local nthreads=$1
    local pps=${PPS_START}
    local best_recv="0"

    log "  Load sweep (step=${PPS_STEP}, miss_threshold=${MAX_MISS_PCT}%):"

    while [[ ${pps} -le ${PPS_MAX} ]]; do
        local run_file="${LOG_SUBDIR}/t${nthreads}_pps${pps}.txt"
        log "    pps=${pps}..."

        [[ -n "${RESULT_FILE_OVERRIDE}" && -f "${run_file}" ]] && rm -f "${run_file}"

        start_server       "${nthreads}"
        copy_meminfo
        start_dpu_experiment

        local client_ok=true
        if ! run_client "${pps}" "${run_file}"; then
            log "    WARNING: client exited with error at pps=${pps}, skipping."
            client_ok=false
        fi

        stop_server || log "    WARNING: stop_server failed (BESS may have crashed); continuing."
        collect_logs "${nthreads}" "${pps}"

        if [[ "${client_ok}" == "false" ]]; then
            pps=$(( pps + PPS_STEP ))
            continue
        fi

        # Validate that the expected summary line is present
        if ! grep -qm1 "^Sent:.*Mpps.*Received:.*Mpps" "${run_file}"; then
            log "    WARNING: could not find throughput summary in output, skipping."
            pps=$(( pps + PPS_STEP ))
            continue
        fi

        local sent_mpps recv_mpps miss_mpps
        sent_mpps=$(parse_mpps  "${run_file}" 2)
        recv_mpps=$(parse_mpps  "${run_file}" 5)
        miss_mpps=$(parse_mpps  "${run_file}" 8)

        local lat_mean lat_p50 lat_p99
        lat_mean=$(parse_latency "${run_file}" "mean")
        lat_p50=$(parse_latency  "${run_file}" "median")
        lat_p99=$(parse_latency  "${run_file}" "99th")

        local retries retries_ex
        retries=$(parse_retries            "${run_file}")
        retries_ex=$(parse_retries_exhausted "${run_file}")

        local host_mpps dpu_mpps
        host_mpps=$(parse_server_mpps "${run_file}" "Host")
        dpu_mpps=$(parse_server_mpps  "${run_file}" "DPU")

        local mpct
        mpct=$(miss_pct "${sent_mpps}" "${recv_mpps}")

        log "    sent=${sent_mpps} Mpps  recv=${recv_mpps} Mpps  miss=${mpct}%  p99=${lat_p99} us  host=${host_mpps} Mpps  dpu=${dpu_mpps} Mpps"

        # Append data point to CSV
        echo "${nthreads},${pps},${sent_mpps},${recv_mpps},${miss_mpps},${mpct},${lat_mean},${lat_p50},${lat_p99},${retries},${retries_ex},${YCSB_WORKLOAD},${KEY_DIST},${ZIPF_THETA},${NUM_KEYS},${host_mpps},${dpu_mpps}" \
            >> "${RESULT_FILE}"

        # Track best (highest) recv throughput for the summary line
        if float_ge "${recv_mpps}" "${best_recv}"; then
            best_recv="${recv_mpps}"
        fi

        # Stop if we have crossed the miss threshold
        if float_ge "${mpct}" "${MAX_MISS_PCT}"; then
            log "    miss% ${mpct}% >= threshold ${MAX_MISS_PCT}% — stopping sweep."
            break
        fi

        pps=$(( pps + PPS_STEP ))
        sleep 5
    done

    log "  Best recv throughput for ${nthreads} thread(s): ${best_recv} Mpps"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
main() {
    log "=== Thread scaling experiment ==="
    log "  min_threads : ${MIN_THREADS}"
    log "  max_threads : ${MAX_THREADS}"
    log "  miss_thresh : ${MAX_MISS_PCT}%"
    log "  pps range   : ${PPS_START}..${PPS_MAX}  step ${PPS_STEP}"
    log "  bench_time  : ${BENCH_TIME}s per step"
    log "  workloads   : ${WORKLOADS[*]}"
    log "  key_dists   : ${KEY_DISTS[*]}$([[ " ${KEY_DISTS[*]} " == *" zipf "* ]] && echo "  zipf_theta=${ZIPF_THETA}")"
    log "  num_keys    : ${NUM_KEYS:-<client default>}"
    log "  warmup      : ${WARMUP}s"
    log "  dpu_only    : ${DPU_ONLY}"
    log "  result dir  : ${RESULT_DIR}"
    echo ""

    mkdir -p "${RESULT_DIR}"
    mkdir -p "$(dirname "${RESULT_FILE}")"
    if [[ ! -f "${RESULT_FILE}" ]]; then
        echo "threads,offered_pps,sent_mpps,recv_mpps,missing_mpps,miss_pct,lat_mean_us,lat_p50_us,lat_p99_us,retries,retries_exhausted,workload,key_dist,zipf_theta,num_keys,host_mpps,dpu_mpps" \
            > "${RESULT_FILE}"
    else
        log "Appending to existing result file: ${RESULT_FILE}"
    fi

    [[ -x "${CLIENT_BIN}" ]] || die "Client binary not found: ${CLIENT_BIN}"

    local summary=()   # "workload key_dist threads peak_mpps" entries for the final report

    for wl in "${WORKLOADS[@]}"; do
        for kd in "${KEY_DISTS[@]}"; do
            YCSB_WORKLOAD="${wl}"
            KEY_DIST="${kd}"
            LOG_SUBDIR="${RESULT_DIR}/ycsb${YCSB_WORKLOAD}_${KEY_DIST}"
            mkdir -p "${LOG_SUBDIR}"

            log "========== workload = ${YCSB_WORKLOAD}  key_dist = ${KEY_DIST} =========="

            for nthreads in $(seq "${MIN_THREADS}" "${MAX_THREADS}"); do
                log "========== threads = ${nthreads} =========="

                [[ "${DPU_ONLY}" == "false" ]] && start_monitor
                sweep_load         "${nthreads}"
                [[ "${DPU_ONLY}" == "false" ]] && stop_monitor
                [[ "${DPU_ONLY}" == "false" ]] && collect_monitor_log "${nthreads}"

                # Record the best recv_mpps for this combination for the summary
                local peak
                peak=$(awk -F, -v t="${nthreads}" -v w="${YCSB_WORKLOAD}" -v d="${KEY_DIST}" \
                    '$1==t && $12==w && $13==d { if ($4+0 > max+0) max=$4 } END { print (max=="") ? "0" : max }' \
                    "${RESULT_FILE}")
                summary+=("workload=${YCSB_WORKLOAD}  key_dist=${KEY_DIST}  threads=${nthreads}  peak_recv=${peak} Mpps")

                if [[ ${nthreads} -lt ${MAX_THREADS} ]]; then
                    log "Cooling down for ${COOLDOWN}s before next thread count..."
                    sleep "${COOLDOWN}"
                fi
                echo ""
            done
        done
    done

    # ---------------------------------------------------------------------------
    # Final summary
    # ---------------------------------------------------------------------------
    log "=== Summary ==="
    for entry in "${summary[@]}"; do
        log "  ${entry}"
    done
    log "Full results: ${RESULT_FILE}"
}

main
