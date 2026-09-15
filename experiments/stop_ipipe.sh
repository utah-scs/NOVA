#!/bin/bash
script_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
bessctl_bin="${script_dir}/../bessctl/bessctl"
num_functions=$1

if [ -z "$num_functions" ]; then
  echo "Usage: $0 <num_functions>"
  exit 1
fi

for i in $(seq 0 $((${num_functions}-1))); do
  port=$((10515+$i))
  $bessctl_bin daemon connect 127.0.0.1:${port} -- daemon stop &> /dev/null && echo "Stopped ipipe${i}" || echo "Failed to stop ipipe${i}"
done
