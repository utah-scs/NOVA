#!/bin/bash

# Script to stop the monitor port
# script from client

DPU_HOST="ubuntu@192.168.100.2"
SCRIPT="monitor_port.py"

echo "Stopping $SCRIPT on DPU ($DPU_HOST)..."

# Check SSH connectivity and stop the process
ssh "$DPU_HOST" "pkill -f '$SCRIPT'"
SSH_EXIT=$?

if [ $SSH_EXIT -eq 255 ]; then
    echo "ERROR: Failed to SSH into $DPU_HOST."
    exit 1
elif [ $SSH_EXIT -eq 1 ]; then
    echo "WARNING: $SCRIPT was not running on $DPU_HOST."
    exit 0
elif [ $SSH_EXIT -ne 0 ]; then
    echo "ERROR: Unexpected error while stopping $SCRIPT (exit code: $SSH_EXIT)."
    exit 1
fi

# Verify the process is actually gone
sleep 1
PID=$(ssh "$DPU_HOST" "pgrep -f '$SCRIPT'")

if [ -n "$PID" ]; then
    echo "ERROR: $SCRIPT (PID: $PID) is still running after pkill."
    exit 1
fi

echo "$SCRIPT stopped successfully."
