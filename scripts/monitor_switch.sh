#!/bin/bash

# This script is used to monitor eswitch packet stats

set -euo pipefail

SECONDS=0
while true; do
  old_pkt=$(./switchctl.sh show_all | grep output:3 | cut -d ' ' -f 5 | cut -d '=' -f 2 | cut -d ',' -f 1)
  old_pkt_time=$SECONDS

  sleep 1

  new_pkt=$(./switchctl.sh show_all | grep output:3 | cut -d ' ' -f 5 | cut -d '=' -f 2 | cut -d ',' -f 1)
  new_pkt_time=$SECONDS

  awk -v old_pkt=$old_pkt -v new_pkt=$new_pkt -v old_time=$old_pkt_time -v new_time=$new_pkt_time 'BEGIN {printf "%f Mpps\n", (new_pkt - old_pkt)/(new_time - old_time)/1000000}'
done
