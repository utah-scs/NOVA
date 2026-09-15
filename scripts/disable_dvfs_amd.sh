#!/usr/bin/env bash
# disable_dvfs_amd.sh
# Disable DVFS on AMD CPUs by forcing maximum performance P-state and
# disabling C-states. Detects which frequency control path is available
# and acts accordingly:
#   - cpufreq driver active (amd_pstate, acpi_cpufreq): use performance governor
#   - no cpufreq driver (bare metal, kernel < 5.17): direct MSR writes

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Error: must run as root (use sudo)" >&2
    exit 1
fi

NCPUS=$(nproc)
echo "System: $NCPUS CPUs detected"

# ── Detect AMD CPU ────────────────────────────────────────────────────────────
if ! grep -q "AuthenticAMD" /proc/cpuinfo; then
    echo "Error: not an AMD CPU" >&2
    exit 1
fi

CPU_FAMILY=$(awk '/cpu family/ {print $NF; exit}' /proc/cpuinfo)
echo "CPU family: $CPU_FAMILY"

# ── Force maximum P-state ─────────────────────────────────────────────────────
if ls /sys/devices/system/cpu/cpu0/cpufreq/ &>/dev/null; then
    # cpufreq driver is active (amd_pstate, acpi_cpufreq, etc.)
    DRIVER=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver 2>/dev/null || echo "unknown")
    echo "cpufreq driver detected: $DRIVER — setting performance governor"
    if ! command -v cpupower &>/dev/null; then
        echo "Error: cpupower not found. Install linux-tools or cpupowerutils." >&2
        exit 1
    fi
    cpupower -c all frequency-set -g performance
    PSTATE_METHOD="cpufreq governor (performance)"
else
    # No cpufreq driver — fall back to direct MSR writes
    # Supported on Family 10h+ (Phenom/Opteron and newer).
    # Family 0Fh (K8) uses a different MSR and is not supported.
    if [[ "$CPU_FAMILY" -lt 16 ]]; then
        echo "Error: CPU family $CPU_FAMILY (K8/earlier) not supported by this script." >&2
        echo "  K8 uses FidVidControl (MSR 0xC0010042) which requires a different approach." >&2
        exit 1
    fi
    if ! command -v wrmsr &>/dev/null; then
        echo "Error: wrmsr not found. Install msr-tools: apt-get install msr-tools" >&2
        exit 1
    fi
    # Ensure the msr kernel module is loaded so /dev/cpu/*/msr devices exist
    if ! ls /dev/cpu/0/msr &>/dev/null; then
        echo "Loading msr kernel module..."
        modprobe msr
    fi
    echo "No cpufreq driver active — writing P0 directly to MSR 0xC0010062 on all CPUs"
    for ((i = 0; i < NCPUS; i++)); do
        wrmsr -p "$i" 0xC0010062 0
    done
    PSTATE_METHOD="direct MSR write (P0)"
fi

# ── Disable C-states (C1, C2, ...) on all CPUs ───────────────────────────────
echo "Disabling C-states on all CPUs..."
for state_dir in /sys/devices/system/cpu/cpu*/cpuidle/state*/; do
    [[ "$(basename "$state_dir")" == "state0" ]] && continue
    echo 1 > "${state_dir}disable"
done

# ── Verify ────────────────────────────────────────────────────────────────────
echo ""
echo "Verification:"
echo "  P-state control method: $PSTATE_METHOD"

if ls /sys/devices/system/cpu/cpu0/cpufreq/ &>/dev/null; then
    echo -n "  Governor on cpu0: "
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
else
    echo -n "  P-state status sample (first 8 CPUs, expect all 0): "
    for ((i = 0; i < NCPUS && i < 8; i++)); do
        rdmsr -p "$i" 0xC0010063 2>/dev/null | tr -d '\n'
        echo -n " "
    done
    echo ""
fi

CSTATE_NAME=$(cat /sys/devices/system/cpu/cpu0/cpuidle/state1/name 2>/dev/null || echo "C1")
CSTATE_DISABLED=$(cat /sys/devices/system/cpu/cpu0/cpuidle/state1/disable 2>/dev/null || echo "n/a")
echo "  $CSTATE_NAME disabled on cpu0: $CSTATE_DISABLED"

echo ""
echo "Done. DVFS disabled — CPUs running at maximum frequency."
echo "Note: settings are not persistent across reboots."
