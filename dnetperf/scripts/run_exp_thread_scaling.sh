#!/usr/bin/env bash
#
# Run thread-scaling experiments for all YCSB workloads (A, B, C) x
# key distributions (zipf, uniform) and collect results into a single CSV.
#
# Usage:
#   ./scripts/run_exp_thread_scaling.sh [options]
#
# Options:
#   --min-threads N      Lowest thread count to test           (default: 1)
#   --max-threads N      Highest thread count to test          (default: 8)
#   --max-miss-pct N     Stop sweep when miss% >= N            (default: 10)
#   --pps-start N        First offered load in pps             (default: 500000)
#   --pps-step N         Load increment per step               (default: 200000)
#   --pps-max N          Hard cap on offered load              (default: 20000000)
#   --bench-time N       Client run duration per step (s)      (default: 10)
#   --cooldown N         Seconds between thread-count iters    (default: 2)
#   --server-wait N      Seconds to wait after server start    (default: 2)
#   --warmup N           Warmup window in seconds              (default: 2)
#   --num-keys N         Key population size                   (default: 64000000)
#   --result-dir DIR     Output directory                      (default: scripts/results/thread_scaling_<timestamp>)
#   --zipf-theta T       Zipf skew exponent                    (default: 0.99)
#   --resume             Resume from --result-dir, skipping completed workload/dist pairs
#   --dpu-only           Disable port-monitor start/stop/collect (propagated to exp script)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXP_SCRIPT="${SCRIPT_DIR}/exp_thread_scaling.sh"

# ---------------------------------------------------------------------------
# Defaults (mirror exp_thread_scaling.sh defaults where applicable)
# ---------------------------------------------------------------------------
MIN_THREADS=1
MAX_THREADS=8
MAX_MISS_PCT=10
PPS_START=500000
PPS_STEP=200000
PPS_MAX=20000000
BENCH_TIME=10
COOLDOWN=2
SERVER_WAIT=2
WARMUP=2
NUM_KEYS=64000000
ZIPF_THETA=0.99
DPU_ONLY=false

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULT_DIR=""
RESUME=false

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
        --warmup)       WARMUP=$2;       shift 2 ;;
        --num-keys)     NUM_KEYS=$2;     shift 2 ;;
        --result-dir)   RESULT_DIR=$2;   shift 2 ;;
        --zipf-theta)   ZIPF_THETA=$2;   shift 2 ;;
        --resume)       RESUME=true;     shift ;;
        --dpu-only)     DPU_ONLY=true;   shift ;;
        -h|--help)      usage ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ "${RESUME}" == true ]]; then
    [[ -z "${RESULT_DIR}" ]] && { echo "ERROR: --resume requires --result-dir" >&2; exit 1; }
    [[ -d "${RESULT_DIR}" ]] || { echo "ERROR: result dir not found: ${RESULT_DIR}" >&2; exit 1; }
fi

[[ -z "${RESULT_DIR}" ]] && RESULT_DIR="${SCRIPT_DIR}/results/thread_scaling_${TIMESTAMP}"
RESULT_FILE="${RESULT_DIR}/results.csv"

mkdir -p "${RESULT_DIR}"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

# ---------------------------------------------------------------------------
# Resume helpers
# ---------------------------------------------------------------------------
# For a given workload+dist, return the highest thread count whose PPS sweep
# is confirmed complete: has a row where miss_pct >= MAX_MISS_PCT or
# offered_pps >= PPS_MAX. Returns 0 if none are complete.
#
# CSV columns (1-indexed):
#   1=threads 2=offered_pps 6=miss_pct 12=workload 13=key_dist
highest_complete_thread() {
    local workload=$1 dist=$2
    [[ -f "${RESULT_FILE}" ]] || { echo 0; return; }
    awk -F, -v wl="${workload}" -v kd="${dist}" \
        -v thresh="${MAX_MISS_PCT}" -v ppsmax="${PPS_MAX}" \
        'NR==1 { next }
         $12==wl && $13==kd {
             t = $1+0; pps = $2+0; miss = $6+0
             if (miss >= thresh || pps >= ppsmax)
                 done[t] = 1
         }
         END {
             max_done = 0
             for (t in done)
                 if (t > max_done) max_done = t
             print max_done
         }' "${RESULT_FILE}"
}

# ---------------------------------------------------------------------------
# Workload × distribution matrix
# ---------------------------------------------------------------------------
WORKLOADS=(A B C)
DISTS=(zipf uniform)

# --min-threads is set per-run when resuming, so it is not in COMMON_ARGS.
COMMON_ARGS=(
    --max-threads   "${MAX_THREADS}"
    --max-miss-pct  "${MAX_MISS_PCT}"
    --pps-start     "${PPS_START}"
    --pps-step      "${PPS_STEP}"
    --pps-max       "${PPS_MAX}"
    --bench-time    "${BENCH_TIME}"
    --cooldown      "${COOLDOWN}"
    --server-wait   "${SERVER_WAIT}"
    --warmup        "${WARMUP}"
    --num-keys      "${NUM_KEYS}"
    --result-dir    "${RESULT_DIR}"
    --result-file   "${RESULT_FILE}"
)
[[ "${DPU_ONLY}" == "true" ]] && COMMON_ARGS+=(--dpu-only)

log "=== Thread-scaling experiment suite ==="
log "  result dir  : ${RESULT_DIR}"
log "  result file : ${RESULT_FILE}"
log "  threads     : ${MIN_THREADS}..${MAX_THREADS}"
log "  pps range   : ${PPS_START}..${PPS_MAX}  step ${PPS_STEP}"
log "  miss thresh : ${MAX_MISS_PCT}%"
log "  num_keys    : ${NUM_KEYS}"
log "  workloads   : ${WORKLOADS[*]}"
log "  dists       : ${DISTS[*]}"
log "  resume      : ${RESUME}"
echo ""

TOTAL=$(( ${#WORKLOADS[@]} * ${#DISTS[@]} ))
RUN=0

for WORKLOAD in "${WORKLOADS[@]}"; do
    for DIST in "${DISTS[@]}"; do
        RUN=$(( RUN + 1 ))
        LOG_FILE="${RESULT_DIR}/exp_${WORKLOAD}_${DIST}.log"
        EFFECTIVE_MIN="${MIN_THREADS}"

        if [[ "${RESUME}" == true ]]; then
            highest=$(highest_complete_thread "${WORKLOAD}" "${DIST}")
            if [[ "${highest}" -ge "${MAX_THREADS}" ]]; then
                log ">>> Run ${RUN}/${TOTAL}: workload=${WORKLOAD}  key_dist=${DIST} — already complete, skipping"
                continue
            elif [[ "${highest}" -gt 0 ]]; then
                EFFECTIVE_MIN=$(( highest + 1 ))
                log ">>> Run ${RUN}/${TOTAL}: workload=${WORKLOAD}  key_dist=${DIST} — resuming from thread ${EFFECTIVE_MIN}"
            else
                log ">>> Run ${RUN}/${TOTAL}: workload=${WORKLOAD}  key_dist=${DIST}"
            fi
        else
            log ">>> Run ${RUN}/${TOTAL}: workload=${WORKLOAD}  key_dist=${DIST}"
        fi
        log "    log: ${LOG_FILE}"
        echo ""

        EXTRA_ARGS=(
            --min-threads "${EFFECTIVE_MIN}"
            --workload    "${WORKLOAD}"
            --key-dist    "${DIST}"
        )
        [[ "${DIST}" == "zipf" ]] && EXTRA_ARGS+=(--zipf-theta "${ZIPF_THETA}")

        "${EXP_SCRIPT}" "${COMMON_ARGS[@]}" "${EXTRA_ARGS[@]}" \
            2>&1 | tee -a "${LOG_FILE}"

        echo ""
        log "<<< Finished run ${RUN}/${TOTAL}: workload=${WORKLOAD}  key_dist=${DIST}"
        echo ""
    done
done

log "=== All runs complete ==="
log "  Results CSV : ${RESULT_FILE}"
log ""
log "  To plot:"
log "    python3 ${SCRIPT_DIR}/plot_thread_scaling.py -f ${RESULT_FILE}"
