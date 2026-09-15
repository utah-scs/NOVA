#!/bin/bash

# Script to start the monitor port
# script from client

DPU_HOST="ubuntu@192.168.100.2"
DPU_DIR="~/bess-nm/scripts"
SCRIPT="python3 -u monitor_port.py"
SCRIPT_ARGS="-y -c 7 -w 500 -r 500 -t 30"

# Run monitoring script on DPU
echo "Starting monitor_port.py on DPU ($DPU_HOST)..."

ssh "$DPU_HOST" "sh -c 'cd $DPU_DIR && nohup $SCRIPT $SCRIPT_ARGS > /tmp/monitor_port.log 2>&1 &'"

if [ $? -ne 0 ]; then
    echo "ERROR: Failed to SSH into $DPU_HOST or launch the script."
    exit 1
fi

# Give the process a moment to start (or crash)
sleep 1

# Verify the process is actually running
PID=$(ssh "$DPU_HOST" "pgrep -f '$SCRIPT $SCRIPT_ARGS'")

if [ -z "$PID" ]; then
    echo "ERROR: monitor_port.py failed to start. Check logs with:"
    echo "  ssh $DPU_HOST 'cat /tmp/monitor_port.log'"
    exit 1
fi

echo "monitor_port.py started successfully (PID: $PID)"
