## One-time Host Setup ##

Install DOCA:

```
bash ${NAAM_DIR}/scripts/doca-host-setup.sh --install-doca
```
Setup host (need to perform everytime after reboot):

```
bash ${NAAM_DIR}/scripts/doca-host-setup.sh --setup-host
```

Setup DPU:

```
bash ${NAAM_DIR}/scripts/doca-host-setup.sh --setup-dpu
```

### Setup hugepages ###

Add following in `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX_DEFAULT="default_hugepagesz=1G hugepagesz=1G hugepages=8"
```

Then run following command:

```
sudo update-grub
```

Now reboot the machine, then mount hugepages:

```
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

## Configuring the DPU ##

The DPU should be set to DPU mode. Run following command:

```
sudo mlxconfig -d /dev/mst/mt41692_pciconf0 s INTERNAL_CPU_MODEL=1 INTERNAL_CPU_PAGE_SUPPLIER=0 INTERNAL_CPU_ESWITCH_MANAGER=0 INTERNAL_CPU_IB_VPORT0=0 INTERNAL_CPU_OFFLOAD_ENGINE=0
```

Then **power cycle** the host.

## Setting up DPU:

Login to DPU:

```
ssh ubuntu@192.168.100.2
```

Add following to `/etc/resolv.conf` on DPU to have internet access:

```
nameserver 8.8.8.8
```

Similar to host setup hugepages in DPU and then power cycle host.

**For BF3: Sometimes 1GB hugepages might not be available use 512MB hugepages**

```
echo 8 | sudo tee /sys/kernel/mm/hugepages/hugepages-524288kB/nr_hugepages
```

## Building NAAM ##

Same on both host and DPU.

```
bash ${NAAM_DIR}/scripts/setup.sh all
```

## Running MICA Experiments ##

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

Change the MICA BESS host script (`${NAAM_DIR}/experiments/MICA_MULTI/server_simple_host.bess`) according to the system topology. Then run following:

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
./run_exp.py -e MICA_MULTI -c experiments/MICA_MULTI/server_simple_dpu.bess -b experiments/MICA_MULTI/mica-naam.c -j -n 1
```

### Configurable hashtable size ###

By default the MICA hashtable is pre-filled with 1M key-value pairs (~48 MB). Use `--num-keys` to override this at run time. The flag recompiles the BPF program with matching `-D` flags and sizes the shared memory region accordingly.

| `--num-keys` | Approx. memory | 1GB hugepages needed (total, shared by all workers) |
|---|---|---|
| 1,000,000 (default) | ~1 GiB | 1 |
| 10,000,000 | ~1 GiB | 1 |
| 64,000,000 | ~3 GiB | 3 |
| 100,000,000 | ~5 GiB | 5 |

**Host — 10M keys:**
```
./run_exp.py -e MICA_MULTI -c experiments/MICA_MULTI/server_simple_host.bess \
    -b experiments/MICA_MULTI/mica-naam.c -j -n 1 --num-keys 10000000
```

**DPU — 100M keys** (BF-2 has 16 GB so this fits):
```
./run_exp.py -e MICA_MULTI -c experiments/MICA_MULTI/server_simple_dpu.bess \
    -b experiments/MICA_MULTI/mica-naam.c -j -n 1 --num-keys 100000000
```

Omitting `--num-keys` (or passing `0`) keeps the compiled-in defaults unchanged.

## Running NAAM B-Tree experiment ##

### Running server on the host ###

Change the B-Tree BESS script (`${NAAM_DIR}/experiments/BPTREE/bplus-search-naam.bess`) according to the system topology. Then run following:

```
./run_exp.py -e BPTREE/ -c BPTREE/bplus-search-naam.bess -b BPTREE/bplus-search-naam.c -j -n 1
```

### Running client ###

To generate 5 Mop/s requests:

```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- 192.168.1.2 10002 10 bpt 32 1 fixed 5000000
```

## Running Monitoring Script ##

`scripts/monitor_port.py` monitors the BESS PMD port stats on either the host or the DPU, detects packet loss, and optionally shifts traffic between them.

```
usage: monitor_port.py [-h] [-s] [-x] [-d] [-y] [-c CPU] [-f STATS_FILE]
                       [-w MS] [-r MS] [-t PCT] [-v]

optional arguments:
  -h, --help                        show this help message and exit
  -s, --host                        Monitor host
  -x, --host-only                   Monitor only host (no DPU signal required)
  -d, --dpu                         Monitor DPU
  -y, --dpu-only                    Monitor only DPU (no host signal sent)
  -c CPU, --cpu CPU                 Pin process to a specific CPU core
  -f FILE, --stats-file FILE        File for recording PMD port stats (CSV)
  -w MS, --wait-init MS             Stabilization wait in ms after traffic starts (default: 5000)
  -r MS, --wait-rule-change MS      Wait in ms after installing a DPU rule change (default: 3000)
  -t PCT, --tolerance PCT           Packet loss tolerance as a percentage, e.g. 1 for 1% (default: 1.0)
  -v, --verbose                     Show high-frequency status messages (retrying, connection lost)
```

### Examples ###

Monitor the DPU port and save stats to a file:
```
python3 scripts/monitor_port.py -d -f /tmp/dpu_stats.csv
```

Monitor the DPU port without coordinating with the host:
```
python3 scripts/monitor_port.py -y -f /tmp/dpu_stats.csv
```

Monitor the host port (waits for a start signal from the DPU):
```
python3 scripts/monitor_port.py -s -f /tmp/host_stats.csv
```

Monitor the host port standalone (no DPU signal required):
```
python3 scripts/monitor_port.py -x -f /tmp/host_stats.csv
```

Pin the monitor process to CPU core 2:
```
python3 scripts/monitor_port.py -d -c 2 -f /tmp/dpu_stats.csv
```

DPU-only with custom waits and 30% loss tolerance, pinned to core 7:
```
python3 scripts/monitor_port.py -y -c 7 -w 500 -r 500 -t 30
```

Show verbose connection status messages:
```
python3 scripts/monitor_port.py -y -v
```
