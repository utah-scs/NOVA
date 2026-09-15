#!/usr/bin/env python3

import os
import sys
import time
import datetime
import subprocess
import argparse
import socket

this_dir = os.path.dirname(os.path.realpath(__file__))
bess_dir = this_dir + '/../'
switchctl_bin = this_dir + '/switchctl.sh'
sys.path.insert(1, bess_dir)

try:
    from pybess.module import *
    from pybess.port import *
    from pybess.bess import *
except ImportError:
    print('Cannot import the API module (pybess)', file=sys.stderr)
    raise

# Name of the BESS pmd port
PORT_NAME = 'pmdport'

# Time units
MICROSEC = 1/1000000
MILLISEC = 1/1000

# Interval for collecting stats
INTERVAL = 10 * MILLISEC

# Waight for EWMA
alpha = 1

# Tolerate % packet loss
TOLERANCE = 0.01

# Initial wait to get stable reading (milliseconds)
WAIT_INIT = 5000

# Wait after rule change (milliseconds)
WAIT_RULE_CHANGE_DPU = 3000

# Shift traffic from host?
# If this is true, then script will
# try to shift traffic from host periodically
SHIFT_TRAFFIC_FROM_HOST = False

# Shift traffic from host every (seconds)
SHIFT_TRAFFIC_FROM_HOST_INTERVAL = 30

# Wait after shift traffic from host (seconds)
WAIT_RULE_CHANGE_HOST = 1

# Current rule file
CUR_RULE_FILE='/tmp/current_rule'

# Socket configuration
# to comunicate with host
HOST_IP = '192.168.100.1'
HOST_PORT = 5000

DPU_IP = '192.168.100.2'

# connect to bess
bess = BESS()

quiet = True

# Miss ratio history
# Keeps NUM_HISTORY number of miss ratio in binary form
# Records 1 if miss ratio is above threshold, 0 otherwise.
# Loss is declared only when the last REQUIRED_CONSECUTIVE readings
# are all 1 — this ignores isolated spikes while detecting sustained
# overload quickly (latency = REQUIRED_CONSECUTIVE * INTERVAL).
NUM_HISTORY = 10
REQUIRED_CONSECUTIVE = 3
CURRENT_INDEX = 0
miss_ratio_history = [0] * NUM_HISTORY

def log(msg):
    if not quiet:
        print(msg)

def wait_for_bess():
    while True:
        try:
            try:
                bess.disconnect()
            except Exception:
                pass
            bess.connect()
            print("{}: Connected to BESS".format(datetime.datetime.now().strftime("%T.%f")))
            return
        except Exception as e:
            log("{}: BESS not available, retrying in {:.3f}s: {}".format(
                datetime.datetime.now().strftime("%T.%f"), INTERVAL, e))
            time.sleep(INTERVAL)

wait_for_bess()

def trigger_rules(command ,subcommand, level=""):
    if len(level) > 0:
        subprocess.call([switchctl_bin, command, subcommand, level])
    else:
        subprocess.call([switchctl_bin, command, subcommand])

# Get all stats
def get_stats():
    while True:
        try:
            old = bess.get_port_stats(PORT_NAME)
            time.sleep(INTERVAL)
            new = bess.get_port_stats(PORT_NAME)
            pkt_per_sec = (new.inc.packets - old.inc.packets) / (new.timestamp - old.timestamp)
            miss_per_sec = (new.inc.dropped - old.inc.dropped) / (new.timestamp - old.timestamp)
            rx_miss = new.inc.dropped - old.inc.dropped
            tx_miss = new.out.dropped - old.out.dropped
            return pkt_per_sec, miss_per_sec, rx_miss, tx_miss
        except Exception as e:
            log("{}: Lost connection to BESS: {}".format(
                datetime.datetime.now().strftime("%T.%f"), e))
            wait_for_bess()

def record_miss_ratio(miss_ratio):
    global CURRENT_INDEX

    if miss_ratio > TOLERANCE:
        miss_ratio_history[CURRENT_INDEX] = 1
    else:
        miss_ratio_history[CURRENT_INDEX] = 0

    CURRENT_INDEX = (CURRENT_INDEX + 1) % NUM_HISTORY

def check_loss():
    # Require REQUIRED_CONSECUTIVE back-to-back overloaded readings.
    # One clean reading resets the streak, avoiding false positives from
    # short transient bursts.
    for i in range(REQUIRED_CONSECUTIVE):
        if miss_ratio_history[(CURRENT_INDEX - 1 - i) % NUM_HISTORY] == 0:
            return False
    return True

# Wait for a period of time
# Dump stats to file
def wait(seconds, exp_start_time, miss_ratio_ewma, f):
    wait_start_time = time.time()
    while time.time() - wait_start_time < seconds:
        pkt_per_sec, miss_per_sec, rx_miss, tx_miss = get_stats()
        
        # Calculate miss ratio
        miss_ratio = cal_miss_ratio(pkt_per_sec, miss_per_sec)

        # Calculate EWMA
        miss_ratio_ewma = alpha * miss_ratio + (1 - alpha) * miss_ratio_ewma

        # Record miss ratio
        record_miss_ratio(miss_ratio_ewma)

        # Dump stats
        dump_stats(f, pkt_per_sec, rx_miss, tx_miss, miss_ratio_ewma, time.time() - exp_start_time)

def create_file_header(f):
    f.write("time,recv_pps,rx_miss,tx_miss,ratio\n")

def dump_stats(f, recv_pps, rx_miss, tx_miss, miss_ratio, time_spent):
    if f is not None:
        f.write("{},{},{},{},{}\n".format(time_spent, recv_pps, rx_miss, tx_miss, miss_ratio))

def cal_miss_ratio(recv_pps, mis_pps):
    if recv_pps == 0:
        return 0
    else:
        return mis_pps / recv_pps

def monitor_dpu_port():
    global CURRENT_INDEX, miss_ratio_history

    print("Started monitoring dpu port")

    trigger_rules("split", "init")

    while True:
        # --- Reset state for each experiment ---
        miss_ratio_ewma = 0
        CURRENT_INDEX = 0
        miss_ratio_history = [0] * NUM_HISTORY

        with open(CUR_RULE_FILE, 'r') as f:
            percentage = int(f.readline())

        # --- Wait for traffic to start ---
        print("{}: Waiting for traffic...".format(datetime.datetime.now().strftime("%T.%f")))
        while True:
            pkt_per_sec, miss_per_sec, rx_miss, tx_miss = get_stats()
            if pkt_per_sec > 10000:
                break

        print("{}: Experiment started".format(datetime.datetime.now().strftime("%T.%f")))
        start_time = time.time()

        # Send experiment start signal to host
        if not monitor_dpu_only:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                s.connect((HOST_IP, HOST_PORT))
            except:
                print("Host monitor script is not running")
            s.close()

        # Wait for stable reading before monitoring
        wait(wait_init, start_time, miss_ratio_ewma, stats_file_handle)

        # Discard any loss recorded during ramp-up so startup drops
        # don't seed the detection window with false positives.
        CURRENT_INDEX = 0
        miss_ratio_history = [0] * NUM_HISTORY

        pkt_per_sec, miss_per_sec, rx_miss, tx_miss = get_stats()
        push_from_host_time = time.time()

        # --- Monitor traffic ---
        while True:
            pkt_per_sec, miss_per_sec, rx_miss, tx_miss = get_stats()

            # Calculate miss ratio
            miss_ratio = cal_miss_ratio(pkt_per_sec, miss_per_sec)
            miss_ratio_ewma = alpha * miss_ratio + (1 - alpha) * miss_ratio_ewma

            # Record miss ratio
            record_miss_ratio(miss_ratio_ewma)

            # Dump stats
            dump_stats(stats_file_handle, pkt_per_sec, rx_miss, tx_miss, miss_ratio_ewma, time.time() - start_time)

            # Traffic stopped - break inner loop and wait for it to restart
            if pkt_per_sec < 1:
                print("{}: Experiment finished".format(datetime.datetime.now().strftime("%T.%f")))
                break

            if check_loss():
                if percentage < 10:
                    percentage += 1
                    print("{}: DPU -> HOST\tTIME {:6.2f}\tHOST {}%".format(
                        datetime.datetime.now().strftime("%T.%f"),
                        time.time() - start_time,
                        percentage * 10))

                    # Shift 10% to host
                    trigger_rules("split", "inc", "1")

                    # Wait after rule install to get stable reading
                    wait(wait_rule_change, start_time, miss_ratio_ewma, stats_file_handle)

            # Push traffic from host to DPU
            if SHIFT_TRAFFIC_FROM_HOST and time.time() - push_from_host_time > SHIFT_TRAFFIC_FROM_HOST_INTERVAL:
                if percentage > 0:
                    percentage -= 1
                    print("{}: HOST -> DPU\tTIME {:6.2f}\tHOST {}%".format(
                        datetime.datetime.now().strftime("%T.%f"),
                        time.time() - start_time,
                        percentage * 10))

                    # Shift 10% to DPU
                    trigger_rules("split", "dec", "1")
                    push_from_host_time = time.time()
                    wait(WAIT_RULE_CHANGE_HOST, start_time, miss_ratio_ewma, stats_file_handle)

# Wait for DPU experiment start signal
# from DPU through socket
def get_dpu_signal():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((HOST_IP, HOST_PORT))
    s.listen(1)
    conn, addr = s.accept()
    print('Got signal from: ', addr)
    conn.close()

def monitor_host_port():
    print("Started monitoring host port")
    
    # Wait for experiment to start
    if monitor_host_only:
        print("Waiting for experiment to start")
        while True:
            pkt_per_sec, miss_per_sec, rx_miss, tx_miss = get_stats()

            if pkt_per_sec > 500000:
                break
    # Wait for DPU experiment start signal
    else:
        print("Waiting for experiment start signal to arrive")
        get_dpu_signal()

    print("{}: Experiment started".format(datetime.datetime.now().strftime("%T.%f")))
    
    start_time = time.time()
    flow_started = False
    while True:
        pkt_per_sec, miss_per_sec, rx_miss, tx_miss = get_stats()

        miss_ratio = cal_miss_ratio(pkt_per_sec, miss_per_sec)
        dump_stats(stats_file_handle, pkt_per_sec, rx_miss, tx_miss, miss_ratio, time.time() - start_time)

        if not flow_started and pkt_per_sec > 5000:
            flow_started = True

        if flow_started and pkt_per_sec < 1:
            print("{}: Experiment finished".format(datetime.datetime.now().strftime("%T.%f")))
            break

def main():
    if monitor_host:
        monitor_host_port()
    elif monitor_dpu:
        monitor_dpu_port()
    else:
        print("Error: Please specify either host or dpu")
        sys.exit(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Monitor host/dpu pmd port')
    parser.add_argument('-s','--host', action='store_true', help='Monitor host')
    parser.add_argument('-x','--host-only', action='store_true', help='Monitor only host')
    parser.add_argument('-d','--dpu', action='store_true', help='Monitor dpu')
    parser.add_argument('-y','--dpu-only', action='store_true', help='Monitor only dpu')
    parser.add_argument('-c','--cpu', type=int, default=-1, help='Pin process to specific CPU core')
    parser.add_argument('-f','--stats-file', type=str, help='File for recording pmd port stats')
    parser.add_argument('-w','--wait-init', type=float, default=WAIT_INIT, help='Initial stabilization wait in milliseconds after traffic starts (default: {})'.format(WAIT_INIT))
    parser.add_argument('-r','--wait-rule-change', type=float, default=WAIT_RULE_CHANGE_DPU, help='Wait in milliseconds after installing a DPU rule change (default: {})'.format(WAIT_RULE_CHANGE_DPU))
    parser.add_argument('-t','--tolerance', type=float, default=TOLERANCE * 100, help='Packet loss tolerance as a percentage, e.g. 1 for 1%% (default: {})'.format(TOLERANCE * 100))
    parser.add_argument('-v','--verbose', action='store_false', dest='quiet', help='Show high-frequency status messages (retrying, connection lost)')

    args = parser.parse_args()
    monitor_host = args.host
    monitor_host_only = args.host_only
    monitor_dpu = args.dpu
    monitor_dpu_only = args.dpu_only
    pin_cpu = args.cpu
    stats_file = args.stats_file
    quiet = args.quiet

    wait_init = args.wait_init / 1000
    wait_rule_change = args.wait_rule_change / 1000
    TOLERANCE = args.tolerance / 100

    if monitor_host_only:
        monitor_host = True

    if monitor_dpu_only:
        monitor_dpu = True

    if monitor_host and monitor_dpu:
        print("\nError: This script can monitor either host or dpu\n")
        parser.print_help()
        sys.exit(1)

    if pin_cpu >= 0:
        print('Pin process {} to cpu {} core'.format(os.getpid(), pin_cpu))
        os.system('taskset -p -c {} {}'.format(pin_cpu, os.getpid()))

    stats_file_handle=None
    if stats_file is not None:
        stats_file_handle = open(stats_file, "w")
        create_file_header(stats_file_handle)

    try:
        main()
    except KeyboardInterrupt:
        print("\nInterrupted, shutting down...")
    finally:
        if stats_file_handle is not None:
            stats_file_handle.flush()
            stats_file_handle.close()
        bess.disconnect()
