# NOVA: A New Framework For Remote Memory Access #

For the evaluation of NOVA, CloudLab node type r7525 is used. All of the experiments below, except outback, require two r7525 nodes. One will be used as a server and another will be used as a client. The outback experiment requires 6 nodes. One will be used as a server and five will be used as clients.

If not mentioned otherwise, CloudLab `node0` will be used as the server and `node1` will be used as the client.

## Setting up the server ##

<details>
<summary>Setting up the server</summary>

### Setting up the server host ###

Install DOCA:

```
bash ${NAAM_DIR}/scripts/doca-host-setup.sh --install-doca
```
Setup host (need to perform every time after reboot):

```
bash ${NAAM_DIR}/scripts/doca-host-setup.sh --setup-host
```

Setup DPU:

```
bash ${NAAM_DIR}/scripts/doca-host-setup.sh --setup-dpu
```

### Setup hugepages ###

Add the following to `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX_DEFAULT="default_hugepagesz=1G hugepagesz=1G hugepages=8"
```

Then run the following command:

```
sudo update-grub
```

Now reboot the machine, then mount hugepages:

```
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

### Configuring the DPU ###

The DPU should be set to DPU mode. Run the following command:

```
sudo mlxconfig -d /dev/mst/mt41686_pciconf0 s INTERNAL_CPU_MODEL=1 INTERNAL_CPU_PAGE_SUPPLIER=0 INTERNAL_CPU_ESWITCH_MANAGER=0 INTERNAL_CPU_IB_VPORT0=0 INTERNAL_CPU_OFFLOAD_ENGINE=0
```

Then **power cycle** the host.

### Setting up DPU:

Login to DPU:

```
ssh ubuntu@192.168.100.2
```

Add the following to `/etc/resolv.conf` on the DPU to have internet access:

```
nameserver 8.8.8.8
```

Similarly to the host setup, set up hugepages on the DPU and then power cycle the host.

**For BF3: Sometimes 1GB hugepages might not be available use 512MB hugepages**

```
echo 8 | sudo tee /sys/kernel/mm/hugepages/hugepages-524288kB/nr_hugepages
```
</details>

### Building NOVA ###

**Building NOVA on server host:**

Clone and build NOVA using the following command:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
mkdir $NOVA_DIR
git clone https://github.com/utah-scs/NOVA $NOVA_DIR
cd $NOVA_DIR
bash ./scripts/setup.sh all
```

**Building NOVA on server DPU:**

Login to DPU:

```
ssh ubuntu@192.168.100.2
```

Clone and build NOVA using the following command:
```
git clone https://github.com/utah-scs/NOVA
cd NOVA
bash ./scripts/setup.sh all
```

## Setting up the client ##

`/proj/sandstorm-PG0` is a shared NFS directory in CloudLab.
After cloning NOVA while setting up the server, NOVA will
be available on the client machine as well (`node1`).

To build the client run the following commands:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
make -C client
```

## Scaling to many functions (Figure-4) ##

To run this experiment run the following command on the client machine `node1`:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
mkdir -p ../ae/figures/fig-4/
bash scripts/exp_ipipe_scaling.sh -o ../ae/figures/fig-4/
```

After the experiment is finished running generated figure can be found in: `${NOVA_DIR}/ae/figures/fig-4/func_scaling.pdf`

## Mitigating Host CPU Interference (Figure-7) ##

## The Impact of Placement (Figure-8) ##

## B+tree Performance (Figure-9) ##

### Running BPT on the server host ###

Run the following command on `node0`:

```
./experiments/run_exp.py -e experiments/BPTREE/ -c experiments/BPTREE/bplus-search-naam-host.bess -b experiments/BPTREE/bplus-search-naam.c -j -n 1
```

Run the following command on the client to get latency/throughput for the server host:

## Comparison with Outback and eRPC (Figure-10) ##
