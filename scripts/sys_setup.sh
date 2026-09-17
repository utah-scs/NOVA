#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0

set -o errexit
set -o pipefail
set -o nounset

SUDO=''
[[ $EUID -ne 0 ]] && SUDO=sudo

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)

# Pseudo-host marker used to fold the local machine into the same
# parallel batches as the remote hosts (see setup_target below).
LOCAL_HOST="(local)"

# Number of hugepages to configure (set via --hugepages); empty means
# hugepages setup is skipped. Read by setup_target/run_on_host so it
# reaches every host in a multi-host run.
HUGEPAGES=""

# Configures $1 hugepages on the current machine.
setup_hugepages() {
  local count=$1
  echo "Setting up $count hugepages..."
  echo "$count" | $SUDO tee /proc/sys/vm/nr_hugepages
}

setup_sys() {
  local hugepages=${1:-}
  local vendor
  vendor=$(grep -m 1 "vendor_id" /proc/cpuinfo)

  $SUDO apt update
  $SUDO apt install -y linux-tools-common linux-tools-"$(uname -r)" htop

  # Install plot packages
  #$SUDO apt install -y python3-pip
  $SUDO apt install -y python3-pandas
  $SUDO apt install -y python3-matplotlib
  pip3 install --break-system-packages plotnine

  if [[ "$vendor" == *"GenuineIntel"* ]]; then
    # Set scaling governor to performance
    $SUDO cpupower frequency-set --governor performance

    # Disable turbo
    echo "1" | $SUDO tee /sys/devices/system/cpu/intel_pstate/no_turbo
  fi

  # Turn off SMP
  echo off | $SUDO tee /sys/devices/system/cpu/smt/control

  if [[ -n "$hugepages" ]]; then
    setup_hugepages "$hugepages"
  fi
}

# Runs the actual setup on the current machine (used both for a plain
# local invocation and as the remote-side entry point when piped over ssh).
run_local() {
  local hugepages=${1:-}
  echo "Running system setup..."
  setup_sys "$hugepages"
}

usage() {
  cat <<EOF
Usage: $0 [-f hosts_file] [-j max_parallel] [--hugepages N] [host ...]

Runs system setup on the local machine, or on one or more remote machines
over SSH when hosts are supplied (either positionally or via -f). Before
setting up anything, passwordless/key-based SSH access is validated on
every host; if any host fails the check, the script aborts without
running on any host. When hosts are given, the local machine is set up
too, in parallel with the remote setups.

  -f hosts_file   File containing one hostname per line (blank lines and
                   lines starting with '#' are ignored)
  -j max_parallel Maximum number of hosts to set up concurrently
                   (default: all hosts at once)
  --hugepages N   Configure N hugepages (vm.nr_hugepages) on every
                   target machine
  -h              Show this help message

With no hosts given, runs on the local machine only.
EOF
}

# Runs setup on a single remote host by piping this script's own source over
# ssh and re-running it there in local mode, so the setup logic only
# lives in one place.
run_on_host() {
  local host=$1
  local remote_cmd="bash -s -- --local"
  [[ -n "$HUGEPAGES" ]] && remote_cmd+=" --hugepages $HUGEPAGES"
  ssh "${SSH_OPTS[@]}" "$host" "$remote_cmd" < "$0"
}

# Dispatches to a plain local setup for the $LOCAL_HOST marker, or an
# ssh-based remote setup otherwise. Lets the local machine share the
# same run_across_hosts batching/parallelism as the remote hosts.
setup_target() {
  local host=$1
  if [[ "$host" == "$LOCAL_HOST" ]]; then
    run_local "$HUGEPAGES"
  else
    run_on_host "$host"
  fi
}

# Verifies that the current host can reach $1 over ssh without a password
# or passphrase prompt (BatchMode=yes in SSH_OPTS fails fast instead of
# hanging on a prompt).
check_host_ssh() {
  local host=$1
  ssh "${SSH_OPTS[@]}" "$host" true
}

# Runs $worker_fn against each of the given hosts, in parallel batches of
# up to $max_parallel, printing each host's captured output as it finishes
# and appending the host to the (by-name) succeeded/failed arrays.
run_across_hosts() {
  local label=$1 worker_fn=$2 max_parallel=$3 succeeded_var=$4 failed_var=$5
  shift 5
  local -n _succeeded=$succeeded_var
  local -n _failed=$failed_var
  local hosts=("$@")
  local idx=0
  local n=${#hosts[@]}

  while [[ $idx -lt $n ]]; do
    local batch_hosts=()
    local batch_pids=()
    local batch_logs=()

    local i=0
    while [[ $i -lt $max_parallel && $idx -lt $n ]]; do
      local host=${hosts[$idx]}
      local log
      log=$(mktemp)
      "$worker_fn" "$host" > "$log" 2>&1 &
      batch_hosts+=("$host")
      batch_pids+=("$!")
      batch_logs+=("$log")
      i=$((i + 1))
      idx=$((idx + 1))
    done

    local j
    for j in "${!batch_pids[@]}"; do
      local host=${batch_hosts[$j]}
      local log=${batch_logs[$j]}
      echo "=== $host ($label) ==="
      cat "$log"
      rm -f "$log"
      if wait "${batch_pids[$j]}"; then
        echo "=== $host: OK ==="
        _succeeded+=("$host")
      else
        echo "=== $host: FAILED ==="
        _failed+=("$host")
      fi
    done
  done
}

main() {
  local hosts=()
  local hosts_file=""
  local max_parallel=0
  local OPTIND=1

  # getopts has no notion of long options, so pull --hugepages (and
  # --hugepages=N) out of the argument list first, leaving the short
  # options for the getopts loop below.
  local args=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --hugepages)
        if [[ $# -lt 2 || ! $2 =~ ^[0-9]+$ ]]; then
          echo "Option --hugepages requires a non-negative integer argument" >&2
          exit 1
        fi
        HUGEPAGES=$2
        shift 2
        ;;
      --hugepages=*)
        HUGEPAGES=${1#*=}
        if ! [[ $HUGEPAGES =~ ^[0-9]+$ ]]; then
          echo "Option --hugepages requires a non-negative integer argument" >&2
          exit 1
        fi
        shift
        ;;
      *)
        args+=("$1")
        shift
        ;;
    esac
  done
  set -- "${args[@]}"

  while getopts ":f:j:h" opt; do
    case "$opt" in
      f) hosts_file=$OPTARG ;;
      j)
        if ! [[ $OPTARG =~ ^[0-9]+$ ]] || [[ $OPTARG -eq 0 ]]; then
          echo "Option -j requires a positive integer" >&2
          exit 1
        fi
        max_parallel=$OPTARG
        ;;
      h) usage; exit 0 ;;
      \?) echo "Unknown option: -$OPTARG" >&2; usage; exit 1 ;;
      :) echo "Option -$OPTARG requires an argument" >&2; usage; exit 1 ;;
    esac
  done
  shift $((OPTIND - 1))

  hosts+=("$@")

  if [[ -n "$hosts_file" ]]; then
    while IFS= read -r line; do
      line="${line%%#*}"
      line="$(echo "$line" | xargs)"
      [[ -z "$line" ]] && continue
      hosts+=("$line")
    done < "$hosts_file"
  fi

  if [[ ${#hosts[@]} -eq 0 ]]; then
    run_local "$HUGEPAGES"
    return
  fi

  [[ $max_parallel -eq 0 ]] && max_parallel=$(( ${#hosts[@]} + 1 ))

  echo "Checking passwordless SSH access to ${#hosts[@]} host(s) (up to $max_parallel in parallel): ${hosts[*]}"

  local ssh_ok=()
  local ssh_failed=()
  run_across_hosts "ssh check" check_host_ssh "$max_parallel" ssh_ok ssh_failed "${hosts[@]}"

  echo
  echo "SSH check summary:"
  for host in "${ssh_ok[@]-}"; do
    [[ -n "$host" ]] && echo "  [OK]     $host"
  done
  for host in "${ssh_failed[@]-}"; do
    [[ -n "$host" ]] && echo "  [FAILED] $host"
  done

  if [[ ${#ssh_failed[@]} -gt 0 ]]; then
    echo
    echo "Aborting: passwordless SSH is not working on ${#ssh_failed[@]} host(s)." >&2
    return 1
  fi

  local setup_hosts=("${hosts[@]}" "$LOCAL_HOST")

  echo
  echo "Running system setup on ${#hosts[@]} remote host(s) and the local machine (up to $max_parallel in parallel): ${hosts[*]}"

  local succeeded=()
  local failed=()
  run_across_hosts "setup" setup_target "$max_parallel" succeeded failed "${setup_hosts[@]}"

  echo
  echo "Setup summary:"
  for host in "${succeeded[@]-}"; do
    [[ -n "$host" ]] && echo "  [OK]     $host"
  done
  for host in "${failed[@]-}"; do
    [[ -n "$host" ]] && echo "  [FAILED] $host"
  done

  [[ ${#failed[@]} -eq 0 ]]
}

(return 2>/dev/null) && echo "Sourced" && return

if [[ "${1:-}" == "--local" ]]; then
  shift
  local_hugepages=""
  if [[ "${1:-}" == "--hugepages" ]]; then
    local_hugepages=${2:-}
  fi
  run_local "$local_hugepages"
else
  main "$@"
fi
