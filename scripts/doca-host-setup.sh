#!/bin/bash

set -o errexit
set -o nounset
set -o pipefail

# Script direcotry
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Host iface providing public internet access
IFACE=$(ip route get 1.1.1.1 | awk '{print $5; exit}')

# DOCA tools version
# Compatible OFED version: 5.7-1.0.2.0
# Compatible kernel version: 5.4.0-200
DOCA_VERSION='3.3.0'
DOCA_REPO_VERSION='3.3.0-088000-26.01-ubuntu2404'

# Version of the DPU bpf os image file
DOCA_BFB_VERSION=3.3.0-202_26.01_ubuntu-24.04_64k

usage() {
  echo -ne "usage: $(basename $0) [ --all | --install-doca |"
  echo " --setup-host | --setup-dpu | --remove-doca ]"
  
  echo "    -a, --all           Setup everything"
  echo "    -i, --install-doca  Install DOCA tools"
  echo "    -s, --setup-host    Setup Host. Run this across host reboots"
  echo "    -d, --setup-dpu     Setup DPU"
  echo "    -r, --remove-doca   Remove DOCA tools"
  echo "    -h, --help          Show this help"
}

# Install doca tools on host
install_doca() {
  sudo apt update
  sudo apt install -y pv

  # Install DOCA on host
  wget https://www.mellanox.com/downloads/DOCA/DOCA_v${DOCA_VERSION}/host/doca-host_${DOCA_REPO_VERSION}_amd64.deb
  sudo dpkg -i doca-host_${DOCA_REPO_VERSION}_amd64.deb
  sudo apt update

  # Install openmp libs
  #sudo apt-get install -y libopenblas-base libopenmpi-dev libomp-dev

  # Create libmpi.so symlink in /usr/lib
  #sudo ln -s /etc/alternatives/libmpi.so-x86_64-linux-gnu /usr/lib/libmpi.so

  sudo apt install -y doca-all

  # Remove unnecessary deb files
  rm doca-host_${DOCA_REPO_VERSION}_amd64.deb

  echo "Restart current shell or source /etc/profile"
}

# Remove doca tools
remove_doca() {
  for f in $(sudo dpkg --list | grep doca | awk '{print $2}' ); do echo $f ; sudo apt remove --purge $f -y ; done

  # Remove doca configs from /etc/profile.d
  sudo rm -f /etc/profile.d/doca-runtime.sh
  sudo rm -f /etc/profile.d/doca-sdk-x86_64-linux-gnu.sh
  sudo rm -f /etc/profile.d/libgrpc.sh
  sudo rm -f /etc/profile.d/mlnx-dpdk-x86_64-linux-gnu.sh

  # Remove symlink
  sudo rm -rf /usr/lib/libmpi.so
}

setup_host() {
  # Enable and start rshim service
  sudo systemctl enable rshim
  sudo systemctl start rshim
  systemctl is-active --wait rshim

  # Wait to bring up the interface
  sleep 5

  # Setup tmfifo interface to access DPU over ssh
  sudo ifconfig tmfifo_net0 192.168.100.1 netmask 255.255.255.252 up

  # Enable IP forwarding on the host
  sudo sysctl -w net.ipv4.ip_forward=1

  # Enable IP forwarding to access internet from DPU
  sudo iptables -t nat -A POSTROUTING -o $IFACE -j MASQUERADE

  # Change firewall rules
  sudo iptables -A FORWARD -i tmfifo_net0 -o $IFACE -j ACCEPT
  sudo iptables -A FORWARD -m state --state ESTABLISHED,RELATED -i $IFACE -j ACCEPT

  # Log in to the DPU and run: echo 'nameserver 1.1.1.1' | sudo tee /etc/resolv.conf
}

setup_dpu() {
  rm -rf /tmp/dpu
  mkdir /tmp/dpu
  pushd /tmp/dpu
  
  # Download DPU bpf os image file
  wget https://content.mellanox.com/BlueField/BFBs/Ubuntu24.04/bf-bundle-${DOCA_BFB_VERSION}_prod.bfb

  # Create CFG file
  # dune3163 (password for entering DPU using `ssh ubuntu@192.168.100.2`)
  echo "ubuntu_PASSWORD='\$1\$RuQTpIja\$AFpVzmHRsMKizGWKj6F/v0'" > bf.cfg
  
  # Flash DPU bpf os image file
  sudo systemctl enable rshim
  sudo systemctl start rshim
  systemctl is-active --wait rshim
  sleep 5
  sudo bfb-install --bfb bf-bundle-${DOCA_BFB_VERSION}_prod.bfb --config bf.cfg --rshim rshim0
  popd
  rm -rf /tmp/dpu
}

# Check number of arguments
if [ $# -eq 0 ]; then
  usage
  exit 1
fi

while [[ $# -gt 0 ]]; do
  case $1 in
    -a|--all)
      install_doca
      setup_host
      setup_dpu
      shift
      ;;
    -i|--install-doca)
      install_doca
      shift
      ;;
    -s|--setup-host)
      setup_host
      shift
      ;;
    -d|--setup-dpu)
      setup_dpu
      shift
      ;;
    -r|--remove)
      remove_doca
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*|--*)
      echo "Uknown option" 1>&2
      echo 1>&2
      usage 1>&2
      exit 1
      ;;
    *)
      echo "Uknown argument" 1>&2
      echo 1>&2
      usage 1>&2
      exit 1
  esac
done
