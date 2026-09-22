# NOVA: A New Framework For Remote Memory Access #

NOVA is a new framework for remote memory access and function offloading. In NOVA, each remote memory operation is an active message: a portable eBPF function packaged with its execution state, which the runtime can suspend mid-call and resume wherever data resides or compute is available, including on a different CPU architecture. The runtime places each message automatically at clients, server-attached SmartNICs, or host CPU cores based on workload and current CPU and network load. For more details please look at our paper.

## Building NOVA ##

Same on both host and DPU.

```
bash ${NOVA_DIR}/scripts/setup.sh all
```

## Building NOVA client ##

```
cd ${NOVA_DIR}/dnetperf
make -C client
```

## Running an example function: MICA ##

### Update config file ###

Check the experiment BESS config file (host and DPU) for the following parameters:

```
/* Should be DPDK port ID of the NIC port being used */
PMD_PORT_ID = 0

/* According to the NUMA node of the NIC starting CPU core ID */
CPU_NUMA_OFFSET=32

/* Distribution of CPU core IDs among NUMA nodes */
CPU_NUMA_ORG = 'linear'

/* PCIe BDF of the NIC port */
PCI_ADDR = '81:00.0'
```

### Running server on the host ###

Change the MICA BESS host script (`${NOVA_DIR}/experiments/MICA_MULTI/server_simple_host.bess`) according to the system topology. Then run following:

```
./run_exp.py -e MICA_MULTI -c experiments/MICA_MULTI/server_simple_host.bess -b experiments/MICA_MULTI/mica-naam.c -j -n 1
```

### Running server on the DPU ###

Run following on the server to copy export information needed for DMA from the DPU:
```
bash scripts/send_meminfo.sh
```

Change the MICA BESS DPU script (`${NAAM_DIR}/experiments/MICA_MULTI/server_simple_dpu.bess`) according to the system topology. Then run following:

```
./run_exp.py -e MICA_MULTI -c experiments/MICA_MULTI/server_simple_dpu.bess -b experiments/MICA_MULTI/mica-naam.c -j -n 6
```

### Running NOVA client to generate hashtable workload ###

```
cd ${NOVA_DIR}/dnetperf
sudo ./client/build/client -l 0,1,2,3,4,5 -- -s 192.168.1.2 -M b8:3f:d2:54:8e:fe -p 10002 -t 10 -T ht --workload C --key-dist uniform -b 32 -n 1 -m fixed -r 1000000 --src-port-start 1234 --src-port-end 1243
```

## Full evaluation guideline ##

For full system setup and evaluation guideline take a look at [artifact evaluation guide.](ae/README.md)
