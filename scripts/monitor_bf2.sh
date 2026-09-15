#!/bin/bash

# Monitor BlueField-2 system health and log periodic snapshots.
# Run this on the BlueField before reproducing the unresponsiveness issue.
# Logs to /var/log/bf2_monitor.log by default.
#
# Usage: ./monitor_bf2.sh [interval_seconds] [logfile]
#   interval_seconds  : Snapshot interval in seconds (default: 30)
#   logfile           : Path to log file (default: /var/log/bf2_monitor.log)
#
# Example: ./monitor_bf2.sh 10 /tmp/bf2_monitor.log

INTERVAL=${1:-30}
LOGFILE=${2:-/var/log/bf2_monitor.log}

log() {
    echo "$@" | tee -a "$LOGFILE"
}

log_section() {
    log ""
    log "=== $1 ==="
}

# Ensure log directory exists
mkdir -p "$(dirname "$LOGFILE")"

log "=========================================="
log "BlueField-2 Monitor started at $(date)"
log "Interval: ${INTERVAL}s | Logfile: ${LOGFILE}"
log "=========================================="

# State for IRQ rate tracking: save previous /proc/interrupts snapshot to a temp file
PREV_INTERRUPTS_FILE=$(mktemp)
trap "rm -f $PREV_INTERRUPTS_FILE" EXIT
FIRST_RUN=1

# State for dmesg: only show entries newer than the previous iteration
PREV_DMESG_TS=$(date -u +"%Y-%m-%dT%H:%M:%S")

while true; do
    log ""
    log "########## $(date) ##########"

    log_section "Uptime / Load"
    uptime | tee -a "$LOGFILE"

    log_section "Memory"
    free -m | tee -a "$LOGFILE"

    log_section "Huge Pages (DPDK/BESS)"
    grep -i huge /proc/meminfo | tee -a "$LOGFILE"

    log_section "CPU Usage (top snapshot)"
    top -bn1 | head -15 | tee -a "$LOGFILE"

    # BF2 exposes temperatures via hwmon, not thermal_zone.
    # Try hwmon first; fall back to thermal_zone if nothing found.
    log_section "Thermal"
    found_thermal=0
    for hwmon_dir in /sys/class/hwmon/hwmon*/; do
        [ -d "$hwmon_dir" ] || continue
        hwmon_name=$(cat "${hwmon_dir}name" 2>/dev/null || echo "hwmon")
        for temp_file in "${hwmon_dir}"temp*_input; do
            [ -f "$temp_file" ] || continue
            label_file="${temp_file%_input}_label"
            label=$(cat "$label_file" 2>/dev/null || basename "$temp_file")
            raw=$(cat "$temp_file" 2>/dev/null) || continue
            echo "  $hwmon_name / $label: $((raw / 1000))°C" | tee -a "$LOGFILE"
            found_thermal=1
        done
    done
    if [ "$found_thermal" -eq 0 ]; then
        for zone in /sys/class/thermal/thermal_zone*/temp; do
            [ -f "$zone" ] || continue
            label=$(cat "$(dirname "$zone")/type" 2>/dev/null || echo "unknown")
            raw=$(cat "$zone" 2>/dev/null) || continue
            echo "  $label: $((raw / 1000))°C" | tee -a "$LOGFILE"
            found_thermal=1
        done
    fi
    [ "$found_thermal" -eq 0 ] && echo "  No thermal sensors found" | tee -a "$LOGFILE"

    # Show only dmesg lines that appeared since the previous iteration.
    # This makes freeze-time kernel messages immediately visible instead of
    # being buried under repeated startup entries.
    log_section "New dmesg entries (since last interval)"
    dmesg --time-format iso --since "$PREV_DMESG_TS" 2>/dev/null \
        | grep -iE "mlx5|error|warn|oom|panic|lockup|rcu stall|aer|pcie|killed" \
        | tee -a "$LOGFILE" \
    || dmesg --time-format iso 2>/dev/null \
        | grep -iE "mlx5|error|warn|oom|panic|lockup|rcu stall|aer|pcie|killed" \
        | tail -20 | tee -a "$LOGFILE"
    PREV_DMESG_TS=$(date -u +"%Y-%m-%dT%H:%M:%S")

    log_section "MLX5 Port Stats"
    if command -v ethtool &>/dev/null; then
        for iface in $(ls /sys/class/net | grep -E "^(p[0-9]|en)"); do
            echo "-- $iface --" | tee -a "$LOGFILE"
            ethtool -S "$iface" 2>/dev/null | grep -iE "error|drop|miss|rx_out_of_buffer" | tee -a "$LOGFILE"
        done
    else
        echo "ethtool not available" | tee -a "$LOGFILE"
    fi

    # Dedicated section for the tmfifo management channel.
    # RX/TX counters stalling here is the earliest sign of an impending freeze.
    log_section "tmfifo_net0 Stats"
    ip -s link show tmfifo_net0 2>/dev/null | tee -a "$LOGFILE" \
        || echo "  tmfifo_net0 not found" | tee -a "$LOGFILE"

    log_section "Network Interfaces"
    ip -s link show 2>/dev/null | grep -A4 "^[0-9]" | tee -a "$LOGFILE"

    log_section "Disk Usage"
    df -h | tee -a "$LOGFILE"

    log_section "Top 5 Processes by CPU"
    ps aux --sort=-%cpu | head -6 | tee -a "$LOGFILE"

    log_section "Top 5 Processes by Memory"
    ps aux --sort=-%mem | head -6 | tee -a "$LOGFILE"

    log_section "Per-Core CPU (mpstat)"
    mpstat -P ALL 1 1 2>/dev/null | tee -a "$LOGFILE"

    log_section "IP Addresses"
    ip addr show | grep -E "^[0-9]|inet " | tee -a "$LOGFILE"

    log_section "Routing Table"
    ip route show | tee -a "$LOGFILE"

    log_section "CPU Isolation (cmdline)"
    cat /proc/cmdline | tee -a "$LOGFILE"

    # Show raw cumulative counts so the log is self-contained.
    log_section "IRQ Affinity (tmfifo + mlx5)"
    grep -E "tmfifo|mlx5|PCI-MSI" /proc/interrupts | tee -a "$LOGFILE"

    # Compute IRQ/sec by diffing the current snapshot against the previous one.
    # Sums across all CPUs (fields 2..NF-4 for an 8-CPU system where the last
    # 4 fields are the interrupt type/number/trigger/name).
    log_section "IRQ Rates (tmfifo + mlx5, delta since last interval)"
    CURR_INTERRUPTS=$(grep -E "tmfifo|mlx5_async|mlx5_sf_ctrl" /proc/interrupts)
    if [ "$FIRST_RUN" -eq 0 ] && [ -s "$PREV_INTERRUPTS_FILE" ]; then
        while IFS= read -r curr_line; do
            irq=$(echo "$curr_line" | awk '{gsub(/:/,""); print $1}')
            name=$(echo "$curr_line" | awk '{print $NF}')
            curr_total=$(echo "$curr_line" | awk '{s=0; for(i=2;i<=NF-4;i++) s+=$i; print s}')
            prev_line=$(grep -E "^ *${irq}:" "$PREV_INTERRUPTS_FILE" || true)
            if [ -n "$prev_line" ]; then
                prev_total=$(echo "$prev_line" | awk '{s=0; for(i=2;i<=NF-4;i++) s+=$i; print s}')
                delta=$((curr_total - prev_total))
                rate=$(awk "BEGIN {printf \"%.1f\", $delta / $INTERVAL}")
                echo "  IRQ $irq ($name): +$delta in ${INTERVAL}s (~${rate}/sec)" | tee -a "$LOGFILE"
            fi
        done <<< "$CURR_INTERRUPTS"
    else
        echo "  (first run — baseline captured, rates shown next interval)" | tee -a "$LOGFILE"
    fi
    echo "$CURR_INTERRUPTS" > "$PREV_INTERRUPTS_FILE"
    FIRST_RUN=0

    log_section "sshd Listening"
    ss -tlnp | grep :22 | tee -a "$LOGFILE"
    systemctl is-active ssh 2>/dev/null | tee -a "$LOGFILE"

    # Show both the process-level affinity mask and each worker thread's mask.
    # The process mask (ff = all cores) is set by DPDK; the per-thread mask
    # shows which cores the workers actually run on.
    log_section "bessd CPU Affinity"
    BESSD_PID=$(pgrep -x bessd 2>/dev/null)
    if [ -n "$BESSD_PID" ]; then
        proc_mask=$(taskset -p "$BESSD_PID" 2>/dev/null | awk '{print $NF}')
        echo "bessd PID $BESSD_PID — process affinity mask: $proc_mask" | tee -a "$LOGFILE"
        echo "  Per-thread affinities:" | tee -a "$LOGFILE"
        for tid_dir in /proc/"$BESSD_PID"/task/*/; do
            tid=$(basename "$tid_dir")
            comm=$(cat "$tid_dir/comm" 2>/dev/null || echo "?")
            mask=$(taskset -p "$tid" 2>/dev/null | awk '{print $NF}')
            echo "    tid $tid ($comm): mask $mask" | tee -a "$LOGFILE"
        done
    else
        echo "bessd not running" | tee -a "$LOGFILE"
    fi

    log_section "OVS Bridge State"
    ovs-vsctl show 2>/dev/null | tee -a "$LOGFILE"

    log_section "OVS Flow Stats"
    BRIDGE=$(ovs-vsctl list-br 2>/dev/null | head -1)
    if [ -n "$BRIDGE" ]; then
        ovs-ofctl dump-flows "$BRIDGE" 2>/dev/null | tee -a "$LOGFILE"
    else
        echo "No OVS bridge found" | tee -a "$LOGFILE"
    fi

    log_section "OVS Coverage (non-zero)"
    ovs-appctl coverage/show 2>/dev/null | grep -v "^  [a-z_].*  0$" | tee -a "$LOGFILE"

    log_section "Ping Host (tmfifo gateway)"
    HOST_GW=$(ip route show dev tmfifo_net0 2>/dev/null | awk '/default/{print $3}')
    if [ -n "$HOST_GW" ]; then
        ping -c2 -W1 "$HOST_GW" 2>&1 | tee -a "$LOGFILE"
    else
        echo "No tmfifo_net0 default gateway found" | tee -a "$LOGFILE"
    fi

    sleep "$INTERVAL"
done
