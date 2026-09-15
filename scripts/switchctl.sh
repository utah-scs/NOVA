#!/usr/bin/env bash

set -euo pipefail

# This script is used to control the eswitch

SUDO=''

PHY_REP=p0          # Physical port representor
HOST_REP=pf0hpf     # Host representor
DPU_REP=en3f0pf0sf0 # DPU representor
BRIDGE=ovsbr1       # Bridge name

# Options for ovs-ofctl
# atomic - Use atomic mode for flow operations
OVS_OFCTL_OPTS=""
OVS_OFCTL_OPTS="$OVS_OFCTL_OPTS --bundle"

# Priority for the mutating operations
# that requires atomic operations
# routed to host
HOST_ATOMIC_PRIO=65500

# Destination port for the mutating operations
# that requires atomic operations
HOST_ATOMIC_DST_PORT=31633

# Current rule file
CUR_RULE_FILE=/tmp/current_rule

# Check if root
if [ "$EUID" -ne 0 ]; then
  SUDO='sudo'
fi

# Send all traffic to host
install_rule_host() {
  $SUDO ovs-ofctl $OVS_OFCTL_OPTS add-flow $BRIDGE "table=0, in_port=${PHY_REP}, priority=65000, actions=output:${HOST_REP}"
  $SUDO ovs-ofctl $OVS_OFCTL_OPTS add-flow $BRIDGE "table=0, in_port=${HOST_REP}, actions=output:${PHY_REP}"
}

# Host interference experiment
# All packets are routed to host first with low priority
install_rule_host_first() {
  $SUDO ovs-ofctl $OVS_OFCTL_OPTS add-flow $BRIDGE "table=0, in_port=${PHY_REP}, priority=32000, actions=output:${HOST_REP}"
  $SUDO ovs-ofctl $OVS_OFCTL_OPTS add-flow $BRIDGE "table=0, in_port=${HOST_REP}, actions=output:${PHY_REP}"
}

# Host interference experiment
# All packets are routed to host first with low priority
# If interference is detected, send all traffic to DPU
install_rule_dpu_second() {
  $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, priority=65000, actions=output:${DPU_REP}"
  $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${DPU_REP}, actions=output:${PHY_REP}"
}

# Send all traffic to DPU
install_rule_dpu() {
  $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, priority=32000, actions=output:${DPU_REP}"
  $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${DPU_REP}, actions=output:${PHY_REP}"
    
  # Forward mutating operations to host
  $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_dst=${HOST_ATOMIC_DST_PORT}, priority=${HOST_ATOMIC_PRIO}, actions=output:${HOST_REP}"
  $SUDO ovs-ofctl $OVS_OFCTL_OPTS add-flow $BRIDGE "table=0, in_port=${HOST_REP}, actions=output:${PHY_REP}"
}

install_rule_default() {
  $SUDO ovs-ofctl add-flow $BRIDGE "priority=0, actions=normal"
}

# Shift load from DPU to host
# Each call to this function shift 10%
shift_from_dpu_to_host() {
  # Check if rule file exists
  if [ ! -f ${CUR_RULE_FILE} ]; then
    echo "Rule file does not exist. Reinitialize."
    exit 1
  fi

  last_rule=$(cat ${CUR_RULE_FILE})
  if [[ "$last_rule" == "0" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1234, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "1" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1235, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "2" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1236, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "3" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1237, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "4" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1238, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "5" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1239, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "6" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1240, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "7" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1241, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "8" ]]; then
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1242, priority=65000, actions=output:${HOST_REP}"
    echo $((last_rule + 1)) > ${CUR_RULE_FILE}
  fi
}

# Shift load from host to DPU
# Each call to this function shift 10%
shift_from_host_to_dpu() {
  # Check if rule file exists
  if [ ! -f ${CUR_RULE_FILE} ]; then
    echo "Rule file does not exist. Reinitialize."
    exit 1
  fi
  
  last_rule=$(cat ${CUR_RULE_FILE})
  if [[ "$last_rule" == "1" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1234, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "2" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1235, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "3" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1236, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "4" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1237, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "5" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1238, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "6" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1239, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "7" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1240, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "8" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1241, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  elif [[ "$last_rule" == "9" ]]; then
    $SUDO ovs-ofctl --strict del-flows $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_src=1242, priority=65000"
    echo $((last_rule - 1)) > ${CUR_RULE_FILE}
  fi
}

rule_split() {
  if [ "$1" == "init" ]; then
    delete_all_rules
    echo 0 > ${CUR_RULE_FILE}
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, priority=32000, actions=output:${DPU_REP}"
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${HOST_REP}, actions=output:${PHY_REP}"
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${DPU_REP}, actions=output:${PHY_REP}"

    # Forward mutating operations to host
    $SUDO ovs-ofctl add-flow $BRIDGE "table=0, in_port=${PHY_REP}, dl_type=0x0800, nw_proto=17, tp_dst=${HOST_ATOMIC_DST_PORT}, priority=${HOST_ATOMIC_PRIO}, actions=output:${HOST_REP}"
  else
    for i in $(seq 1 $2); do
      if [ "$1" == "inc" ]; then
        shift_from_dpu_to_host
      elif [ "$1" == "dec" ]; then
        shift_from_host_to_dpu
      fi
    done
  fi
}

# Sow current load split in percentage
show_cur_split() {
  if [ ! -f ${CUR_RULE_FILE} ]; then
    echo "Rule file does not exist. Reinitialize split."
    exit 1
  fi
  
  echo "Current load split:"
  echo "  DPU:  $(((10 - $(cat ${CUR_RULE_FILE})) * 10))%"
  echo "  Host: $(($(cat ${CUR_RULE_FILE}) * 10))%"
}

toggle_rule() {
  cur_port=$(sudo ovs-ofctl dump-flows ovsbr2 | grep in_port=1 | awk '{print $8}' | cut -d : -f 2)
  if [ "$cur_port" == "3" ]; then
    install_rule_host
  else
    install_rule_dpu
  fi
}

delete_all_rules() {
  $SUDO ovs-ofctl del-flows $BRIDGE
}

show_all_rules() {
  $SUDO ovs-ofctl dump-flows $BRIDGE
}

check_offload() {
  $SUDO ovs-appctl dpctl/dump-flows -m
}

usage() {
  APP_NAME=$(basename $0)
  echo "Usage: $APP_NAME - Install/Modify/Show eSwitch rules"
  echo ""
  echo "  $APP_NAME host                            - Install rule to send all traffic to host"
  echo "  $APP_NAME dpu                             - Install rule to send all traffic to DPU"
  echo "  $APP_NAME default                         - Install factory default eSwitch rule"
  echo "  $APP_NAME split init                      - Initialize traffic split rules"
  echo "  $APP_NAME split inc N                     - Shift N% traffic from DPU to host"
  echo "  $APP_NAME split dec N                     - Shift N% traffic from host to DPU"
  echo "  $APP_NAME split show                      - Show current traffic split"
  echo "  $APP_NAME show_all                        - Show all rules"
  echo "  $APP_NAME show_offload                    - Show hw offloads"
  echo "  $APP_NAME delete_all                      - Delete all rules"
  exit 1
}

# Check number of arguments
if [ $# -lt 1 ]; then
  usage
fi

case $1 in
  host)
    delete_all_rules
    #install_rule_host
    install_rule_host_first
    ;;
  dpu)
    delete_all_rules
    install_rule_dpu
    #install_rule_dpu_second
    ;;
  default)
    delete_all_rules
    install_rule_default
    ;;
  split)
    if [ "$2" == "init" ]; then
      rule_split $2
    elif [ "$2" == "inc" ]; then
      rule_split $2 $3
    elif [ "$2" == "dec" ]; then
      rule_split $2 $3
    elif [ "$2" == "show" ]; then
      show_cur_split
    else
      echo "Unknown split option $2"
    fi
    ;;
  show_all)
    show_all_rules
    ;;
  show_offload)
    check_offload
    ;;
  delete_all)
    delete_all_rules
    ;;
  *)
    usage
    ;;
esac
