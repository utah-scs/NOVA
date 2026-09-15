#!/bin/bash

set -euo pipefail

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

IF="enp4s0f0"

DPDK_VERSION=20.11-rc4
DPDK_PATH=http://git.dpdk.org/dpdk/snapshot
RTE_SDK=$PWD/dpdk-${DPDK_VERSION}

if [ "$EUID" -ne 0 ]; then
    echo "not running as root, using sudo"
    APT="sudo apt-get"
    SUDO="sudo"
else
    echo "running as root."
    APT="apt-get"
    SUDO=""
fi

install_deps() {
	$APT update
	$APT -y install pip ninja-build meson \
		python3-pip libnuma-dev
	pip install pyelftools pandas matplotlib
	echo msttcorefonts msttcorefonts/accepted-mscorefonts-eula select true | sudo debconf-set-selections

	# TODO: Make this scriptable
	#sudo apt-get -y install msttcorefonts
	#rm ~/.cache/matplotlib -rf
}

install_dpdk() {
    echo "Installing DPDK"

    ## Download DPDK version.
    wget ${DPDK_PATH}/dpdk-${DPDK_VERSION}.tar.gz -O - | tar xz -C /tmp
    cp -r /tmp/dpdk-${DPDK_VERSION} ${RTE_SDK}

    ## Build DPDK and install into system paths.
    ## More info: https://doc.dpdk.org/guides/prog_guide/build-sdk-meson.html.
    pushd ${RTE_SDK} && $SUDO meson build
    pushd build && $SUDO ninja && $SUDO ninja install
    popd
    popd
    $SUDO ldconfig
    $SUDO rm -rf ${RTE_SDK}/build /tmp/dpdk-*
}

# setup hugepages
setup_hugepages() {
	hugepages=2048
	for node in /sys/devices/system/node/node*; do
		echo $hugepages | sudo tee ${node}/hugepages/hugepages-2048kB/nr_hugepages
	done
}

# Check number of arguments
if [ $# -ne 1 ]; then
	echo "Usage: $0 [deps | dpdk | hugepages | bind | all]"
	exit 1
fi

case "$1" in
	"deps")
		install_deps
		;;
	"dpdk")
		install_dpdk
		;;
	"hugepages")
		setup_hugepages
		;;
	"bind")
		${SCRIPT_DIR}/bind-nic.sh "$IF"
		;;
	"all")
		#install_deps
		install_dpdk
		#setup_hugepages
		#${SCRIPT_DIR}/bind-nic.sh "$IF"
		;;
	*)
		echo "Usage: $0 [deps | dpdk | hugepages | bind | all]"
		exit 1
		;;
esac
