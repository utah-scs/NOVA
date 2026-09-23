# NOVA: A New Framework For Remote Memory Access #

For the evaluation of NOVA, CloudLab node type r7525 is used. All of the experiments below, except outback, require two r7525 nodes. One will be used as a server and another will be used as a client. The outback experiment requires 6 nodes. One will be used as a server and five will be used as clients.

If not mentioned otherwise, CloudLab `node0` will be used as the server and `node1` will be used as the client.

## Setting up the server ##

<details>
<summary>Setting up the server</summary>

### Setting up the server host ###

Install DOCA:

```
bash ${NOVA_DIR}/scripts/doca-host-setup.sh --install-doca
```
Setup host (need to perform every time after reboot):

```
bash ${NOVA_DIR}/scripts/doca-host-setup.sh --setup-host
```

Setup DPU:

```
bash ${NOVA_DIR}/scripts/doca-host-setup.sh --setup-dpu
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

Finally run following script on `node0` to install necessary packages and trun off DVFS,SMP on both client and server:
```
bash ${NOVA_DIR}/scripts/sys_setup.sh node1
```

</details>

### Building NOVA ###

**Building NOVA on server host:**

Clone and build NOVA using the following command on `node0`:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
mkdir $NOVA_DIR
git clone https://github.com/utah-scs/NOVA $NOVA_DIR
cd $NOVA_DIR
bash ./scripts/setup.sh all
```

**Building NOVA on server DPU:**

Login to `node0` DPU. Run following comman on `node0`:

```
ssh ubuntu@192.168.100.2
```

Clone and build NOVA using the following command on DPU:
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

**Estimated run time: 2 hours**

To run this experiment run the following command on the client machine `node1`:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR/dnetperf
mkdir -p ../ae/figures/fig-4/
bash scripts/exp_ipipe_scaling.sh -o ../ae/figures/fig-4/ -d $NOVA_DIR_DPU
```

After the experiment is finished running generated figure can be found in: `${NOVA_DIR}/ae/figures/fig-4/func_scaling.pdf`

## Mitigating Host CPU Interference (Figure-7) ##

**Estimated run time: 10 minutes**

To run this experiment run the following command on the client machine `node1`:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR/dnetperf
mkdir -p ../ae/figures/fig-7/
./scripts/exp_host_interference.sh --result-dir ../ae/figure/fig-7/ -d $NOVA_DIR_DPU
```

Generated figures can be found in `${NOVA_DIR}/ae/figures/fig-7/`

## The Impact of Placement (Figure-8) ##

**Estimated run time: 2 hours**

### (Step 1) Getting latency/throughput for Host ###

Run MICA function on the server host. Run following command on `node0`:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR
./experiments/run_exp.py -e experiments/MICA_MULTI/ -c experiments/MICA_MULTI/server_simple_host.bess -b experiments/MICA_MULTI/mica-naam.c -j -n 1
```

Run the client script on `node1`:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
mkdir -p ../ae/figure/fig-8/
bash scripts/run_ht_exp.sh -o ../ae/figure/fig-8/host.csv -b host
```

### (Step 2) Getting latency/throughput for Host + NIC ###

Make sure MICA function is running on server host from previous step. Send the host memory information to DPU:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR
./scripts/send_meminfo.sh
```

Log in to DPU, run MICA function and start monitoring for automatic offloading on DPU:
```
ssh ubuntu@192.168.100.2
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR_DPU
./experiments/run_exp.py -e experiments/MICA_MULTI/ -c experiments/MICA_MULTI/server_simple_dpu.bess -b experiments/MICA_MULTI/mica-naam.c -j -n 6
./scripts/monitor_port.py -y -c 7 -w 500 -r 500 -t 10
```

Run the client script on `node1`:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
bash scripts/run_ht_exp.sh -o ../ae/figure/fig-8/host_dpu.csv -b host_dpu
```

### (Step 3) Running MICA in client mode ###

Checkout to one-sided branch and build NOVA on both server and client machine on `node0` and `node1`:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR
git checkout one-sided
bash ./scripts/setup.sh all
```

Now run the MICA function in client machine on `node1`:
```
./experiments/run_exp.py -e experiments/CLIENT_REG_MULTINODE/ -c experiments/CLIENT_REG_MULTINODE/test_vhost_ubpf.bess -b experiments/MICA_MULTI/mica-naam.c -j -n 1
```

Now run server-side DMA engine for the hashtable on `node0`:
```
./experiments/run_exp.py -e experiments/CLIENT_REG_MULTINODE/ -c experiments/CLIENT_REG_MULTINODE/client_side_naam_mica.bess -n 1
```

Set `dpdk_port` to 0 in @dnetperf/client/client.c line 115 and build the client. 
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
make -C client
```

Now run the client in vdev mode on `node1`:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
bash scripts/run_ht_exp_vdev.sh -o ../ae/figure/fig-8/client.csv -b client
```

Run plot script on `node1` to generate the plot:
```
git checkout main
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
python3 scripts/plot_function_placement.py -d ../ae/figure/fig-8/
```

Generated plot can be found in `${NOVA_DIR}/ae/figures/fig-8/fig8_function_placement.pdf`

## B+tree Performance (Figure-9) ##

**Estimated run time: 2 hours**

### (Step 1) Running BPT on the server host ###

Run the following command on `node0` to start the server with B+ tree function running:

```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR
./experiments/run_exp.py -e experiments/BPTREE/ -c experiments/BPTREE/bplus-search-naam-host.bess -b experiments/BPTREE/bplus-search-naam.c -j -n 1
```

Login to `node0` DPU and direct all flow to the server host using following command on `node0`:
```
ssh ubuntu@192.168.100.2
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR_DPU
./scripts/switchctl.sh host
exit
```

Run the following command on the client (`node1`) to get latency/throughput for the server host:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
mkdir -p ../ae/figure/fig-9/
bash scripts/run_bpt_exp.sh -o ../ae/figure/fig-9/bpt-host.csv -b host
```

### (Step 2) Running BPT on the DPU ###

Make sure server is already running on the server host(`node0`) from the previous step.

Send B+ tree memory region information to the DPU for DMA. Run following command on `node0`:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR
./scripts/send_meminfo.sh
```

Login to `node0` DPU, start B+ tree server functino on DPU and direct all flow to the DPU using following commands:
```
ssh ubuntu@192.168.100.2
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR_DPU
./scripts/switchctl.sh dpu
./experiments/run_exp.py -e experiments/BPTREE/ -c experiments/BPTREE/bplus-search-naam-dpu.bess -b experiments/BPTREE/bplus-search-naam.c -j -n 6
```

Run the following command on the client (`node1`) to get latency/throughput for the server DPU:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
bash scripts/run_bpt_exp.sh -o ../ae/figure/fig-9/bpt-dpu.csv -b dpu 
```

### (Step 3) Running BPT on the DPU as cache ###

Login to `node0` DPU, start B+ tree server functino on DPU as cache and direct all flow to the DPU using following commands:
```
ssh ubuntu@192.168.100.2
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR_DPU
./scripts/switchctl.sh dpu
./experiments/run_exp.py -e experiments/BPTREE/ -c experiments/BPTREE/bplus-search-naam-dpu-cache.bess -b experiments/BPTREE/bplus-search-naam.c -j -n 6
```

Run the following command on the client (`node1`) to get latency/throughput for the server DPU as cache:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
bash scripts/run_bpt_exp.sh -o ../ae/figure/fig-9/bpt-dpu-cache.csv -b dpu_cache
```

### (Step 4) Running RDMA BPT ###

Stop bessd running on the host server on `node0` run following command:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR
./bessctl/bessctl daemon stop
```

Login to `node0` DPU, stop bessd running on DPU and direct all flow to the host using following commands:
```
ssh ubuntu@192.168.100.2
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR_DPU
./bessctl/bessctl daemon stop
./scripts/switchctl.sh default
```

Clone RDMA B+ tree implementation on `node0` (Will be also available on the same directory on `node1`):
```
RDMA_DIR=/proj/sandstorm-PG0/eurosys-ae/rdma-bpt
mkdir $RDMA_DIR
git clone https://github.com/aagontuk/rdma-bpt $RDMA_DIR
```

Run the RDMA server on `node0` using following commands:
```
RDMA_DIR=/proj/sandstorm-PG0/eurosys-ae/rdma-bpt
cd $RDMA_DIR
numactl --cpunodebind=1 --membind=1 ./server
```

From the server run record the address of the root. Find similar line from the terminal:
```
root addr: 0x767b587f9905 (**CHANGE CLIENT ACCORDINGLY**)
```

On `node1` update `ROOT_ADDR` variable in `client.c` and replace with the recorded address and build the client:
```
RDMA_DIR=/proj/sandstorm-PG0/eurosys-ae/rdma-bpt
cd $RDMA_DIR
vim client.c
make
```

Then on `node1` run following command to record RDMA B+ tree latency/throughput (make sure RDMA server is running on `node0`):
```
RDMA_DIR=/proj/sandstorm-PG0/eurosys-ae/rdma-bpt
cd $RDMA_DIR
./run_exp.sh -o ../NOVA/ae/figure/fig-9/bpt-rdma.csv -b rdma
```

Finally run the following command on `node1` to generate the plot from the collected data:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
python3 scripts/plot_btree.py --dpu ../ae/figure/fig-9/bpt-dpu.csv --dpu-cache ../ae/figure/fig-9/bpt-dpu-cache.csv --host ../ae/figure/fig-9/bpt-host.csv --rdma ../ae/figure/fig-9/bpt-rdma.csv -o ../ae/figure/fig-9
```

Generated figure can be found in: `${NOVA_DIR}/ae/figures/fig-9/bpt_tput_lat_small.pdf` and `${NOVA_DIR}/ae/figures/fig-9/bpt_tx_bw_small.pdf`

## Comparison with Outback and eRPC (Figure-10) ##

**Estimated run time: 1 day**

### (Step 1) Getting Host, DPU and combined results ###

Run following commands from `node1`:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
mkdir -p ../ae/figure/fig-10/host
mkdir -p ../ae/figure/fig-10/dpu
./scripts/exp_thread_scaling.sh --max-threads 8 --max-miss-pct 10 --pps-max 10000000 --result-dir ../ae/figure/fig-10/host --result-file ../ae/figure/fig-10/thread_scaling_host.csv --num-keys 64000000 --workload A,B,C --key-dist uniform,zipf; ./scripts/exp_thread_scaling.sh --max-threads 8 --max-miss-pct 10 --pps-max 10000000 --result-dir ../ae/figure/fig-10/dpu --result-file ../ae/figure/fig-10/thread_scaling_dpu.csv --num-keys 64000000 --workload A,B,C --key-dist uniform,zipf --dpu-only
```

### (Step 2) Getting eRPC throughput and latency results ###

Log in to `node0` DPU and set the flow rule to default mode:
```
ssh ubuntu@192.168.100.2
NOVA_DIR_DPU=/home/ubuntu/NOVA
cd $NOVA_DIR_DPU
./bessctl/bessctl daemon stop
./scripts/switchctl.sh default
```

Clone and build eRPC on `node0`:

```
ERPC_DIR=/proj/sandstorm-PG0/eurosys-ae/eRPC
git clone https://github.com/aagontuk/erpc-mica $ERPC_DIR
cd $ERPC_DIR
mkdir build_mica && cd build_mica
cmake .. -DPERF=ON -DTRANSPORT=dpdk -DAPP=mica_server
make -j $(nproc)
```

Run following commands from `node0`:
```
ERPC_DIR=/proj/sandstorm-PG0/eurosys-ae/eRPC
cd $ERPC_DIR
./scripts/mica_sweep.sh --grid --num-server-threads 8 --num-keys 64000000 --max-client-threads 32
  --test-ms 5000 --csv-out ../NOVA/ae/figure/fig-10/erpc.csv
```

### (Step 3) Getting outback throughput and latency results ###

**Note: Running outback experiment requires 6 r7525 CloudLab nodes with 100 Gbps connectivity. 1 node as the server machine and 5 nodes as clients. But only three is available for the artifact evaluation from CloudLab. So it won't be possible to replicate the full outback line as shown in the paper**

Clone and build outback on `node0`:
```
OUTBACK_DIR=/proj/sandstorm-PG0/eurosys-ae/outback
git clone https://github.com/aagontuk/outback $OUTBACK_DIR
cd $OUTBACK_DIR
bash setup-env.sh
bash build.sh
```

Install necessary libraries to run outback clinet on `node1`:
```
OUTBACK_DIR=/proj/sandstorm-PG0/eurosys-ae/outback
cd $OUTBACK_DIR
bash setup-env.sh
```

Run experiment script on `node0`:
```
OUTBACK_DIR=/proj/sandstorm-PG0/eurosys-ae/outback
cd $OUTBACK_DIR
mkdir -p ../NOVA/ae/figure/fig-10/outback
./run-bench.sh --min-server-threads=1 --max-server-threads=8 --client-threads=8,16,32 --client-nodes=node1 --client-nic-idx=3 --server-nic-idx=2 --numa-node=1 --server-addr=10.10.1.1:8888 --results-dir=../NOVA/ae/figure/fig-10/outback/
```

After getting all the results run the plot script using following command:
```
NOVA_DIR=/proj/sandstorm-PG0/eurosys-ae/NOVA
cd $NOVA_DIR/dnetperf
python3 scripts/plot_thread_scaling_facets.py --dpu-host ../ae/figure/fig-10/thread_scaling_host.csv --dpu ../ae/figure/fig-10/thread_scaling_dpu.csv --outback ../ae/figure/fig-10/outback/throughput.csv --erpc ../ae/figure/fig-10/erpc.csv -o ../ae/figure/fig-10/fig-10.pdf
```

Generated figure can be found in: `${NOVA_DIR}/ae/figures/fig-10/fig-10.pdf`
