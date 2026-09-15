# Script for generating interface load
# Execute it with: taskset -c $CORE ./gen_interference.sh

#!/bin/bash

# Print process affinity list
taskset -cp $$

# infinite loop
while true; do
  :
done
