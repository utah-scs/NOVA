#!/bin/bash

set -e

DPDK_VERSION=20.11-rc4
RTE_SDK=$(pwd)/dpdk-${DPDK_VERSION}

MLX_OFED_VERSION="5.7-1.0.2.0"

IF=""
if [ ! -z "$1" ]; then
	IF="$1"
else
	echo "Please provide a correct interface name"
	exit 1
fi

MAC=`ifconfig $IF | grep ether | cut -d " " -f10`
PCI=`ethtool -i $IF | grep bus |cut -d " " -f2`

# Check if the NIC is a Mellanox NIC
# If it is, then bind isn't needed as
# they use bifurcated driver
# Install Mellanox OFED driver instead
IS_MLNX=`lspci -s $PCI | grep Mellanox > /dev/null && echo "true" || echo "false"`
IS_INTEL=`lspci -s $PCI | grep Intel > /dev/null && echo "true" || echo "false"`

#
# Unloads igb_uio.ko.
#
remove_igb_uio_module()
{
    echo "Unloading any existing DPDK UIO module"
    sudo /sbin/rmmod igb_uio | true
}

install_kmods() {
    Module=dpdk-kmods
    Hash="e721c733cd24206399bebb8f0751b0387c4c1595"
    Kmod_path=https://git.dpdk.org/${Module}/snapshot/${Module}-${Hash}.tar.gz
    wget $Kmod_path -O ${Module}.tar.gz
    mkdir -p ${Module}
    tar xzf ${Module}.tar.gz -C ./${Module} --strip-components 1
    pushd $Module/linux/igb_uio
    make
    sudo insmod igb_uio.ko
    popd
    rm ${Module}.tar.gz | true
}

#
# Loads new igb_uio.ko (and uio module if needed).
#
load_igb_uio_module()
{
    remove_igb_uio_module
    sudo /sbin/modprobe uio

    # UIO may be compiled into kernel, so it may not be an error if it can't
    # be loaded.

    echo "Loading DPDK UIO module"
    install_kmods
    if [ $? -ne 0 ] ; then
        echo "## ERROR: Could not load kmod/igb_uio.ko."
        exit 1
    fi
}

bind_devices_to_igb_uio()
{
	PCI_PATH=$PCI
	if [ ${PCI_PATH} ]; then
		sudo ifconfig $IF down
		if [ -d /sys/module/igb_uio ]; then
			sudo ${RTE_SDK}/usertools/dpdk-devbind.py -u $PCI_PATH && echo "Unbind device $PCI_PATH."
			sudo ${RTE_SDK}/usertools/dpdk-devbind.py -b igb_uio $PCI_PATH && echo "Bind device $PCI_PATH to igb_uio driver."
			${RTE_SDK}/usertools/dpdk-devbind.py --status
		else
			echo "# Please load the 'igb_uio' kernel module before querying or "
			echo "# adjusting device bindings"
		fi
	fi
}

install_mlx_ofed() {
    mkdir /tmp/mlx
    pushd /tmp/mlx
    curl -L https://content.mellanox.com/ofed/MLNX_OFED-${MLX_OFED_VERSION}/MLNX_OFED_LINUX-${MLX_OFED_VERSION}-ubuntu20.04-x86_64.tgz | \
            tar xz -C . --strip-components=2
    sudo ./mlnxofedinstall --with-mft --vma --with-mstflint --auto-add-kernel-support --without-fw-update --dpdk --upstream-libs --force
    popd

    rm -rf /tmp/mlx
}

if [ "$IS_MLNX" == "true" ]; then
    echo "## Installing Mellanox OFED driver"
    install_mlx_ofed
elif [ "$IS_INTEL" == "true" ]; then
    echo "## Binding $PCI to igb_uio driver"
    load_igb_uio_module
    bind_devices_to_igb_uio
else
	echo "Unknow NIC vendor"
fi
