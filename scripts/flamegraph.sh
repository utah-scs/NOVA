#!/usr/bin/env bash
set -euo pipefail

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

# Name of this script
SCRIPT_NAME=$(basename $0)

# Script group name
GROUP=$(ls -l ${SCRIPT_DIR} | grep ${SCRIPT_NAME} | awk '{print $4}')

# Check argument
if [ $# -ne 1 ]; then
    echo "Usage: $0 <name>"
    exit 1
fi

NAME=$1

FLAME_GRAPH_LOC=$HOME/FlameGraph
RESULT_DIR=${SCRIPT_DIR}/flamegraphs/${NAME}

# Create result directory
# If exist, remove it
if [ -d ${RESULT_DIR} ]; then
    rm -rf ${RESULT_DIR}
fi

mkdir -p ${RESULT_DIR}

# Collect perf
sudo perf record -a -g -o ${RESULT_DIR}/perf.data -- sleep 60

# Chang ownership from root to ubuntu
sudo chown ${USER}:${GROUP} ${RESULT_DIR}/perf.data

# perf script
perf script -i ${RESULT_DIR}/perf.data > ${RESULT_DIR}/${NAME}.perf

# stack collaps
${FLAME_GRAPH_LOC}/stackcollapse-perf.pl ${RESULT_DIR}/${NAME}.perf > ${RESULT_DIR}/${NAME}.folded

# flame graph
${FLAME_GRAPH_LOC}/flamegraph.pl ${RESULT_DIR}/${NAME}.folded > ${RESULT_DIR}/${NAME}.svg
