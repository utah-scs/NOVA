# Performance Benchmarks

## Installing build requirements ##

```
sudo apt install meson ninja-build pkg-config python3-pip
pip install pyelftools
```

* Install Mellanox OFED 5.7-1.0.2.0

## Installing DPDK ##

```
git clone https://dpdk.org/git/dpdk
cd dpdk
git checkout v21.11
meson -Ddisable_drivers=common/octeontx,common/octeontx2 build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

## Building

```
git submodule update --init --recursive
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/usr/local/lib/x86_64-linux-gnu/pkgconfig/
make -C client
make -C server
```

## Setup Hugepages ##

```
sudo vim /etc/default/grub
GRUB_CMDLINE_LINUX_DEFAULT="default_hugepagesz=1G hugepagesz=1G hugepages=8"
sudo update-grub
reboot
```

## Run Client

```
sudo ./client/build/client [EAL args] -- [options]
```

### Required Options ###

| Short | Long | Description |
|-------|------|-------------|
| `-s <ip>` | `--server <ip>` | Server IP address |
| `-M <mac>` | `--server-mac <mac>` | Server MAC address (format: `xx:xx:xx:xx:xx:xx`) |
| `-p <port>` | `--port <port>` | Server UDP port |
| `-t <seconds>` | `--time <seconds>` | Run duration (0 = unlimited) |
| `-T <type>` | `--type <type>` | Payload type: `get`, `set`, `list`, `ht`, `bpt` |
| `-b <n>` | `--batch <n>` | TX batch size |
| `-n <n>` | `--funcs <n>` | Number of server functions |
| `-m <type>` | `--loadgen <type>` | Load generator: `fixed`, `variable`, `ladder` |
| `-r <pps>` | `--pps <pps>` | Offered packets per second |

### Variable / Ladder Mode (all three required) ###

| Short | Long | Description |
|-------|------|-------------|
| `-e <pps>` | `--pps-end <pps>` | PPS end value |
| `-k <pps>` | `--pps-step <pps>` | PPS step per interval |
| `-i <seconds>` | `--pps-time <seconds>` | Seconds per step |

### Optional Options ###

| Short | Long | Description |
|-------|------|-------------|
| `-o <file>` | `--output <file>` | Per-packet stats output file |
| | `--src-port-start <port>` | First source UDP port (default: 1234) |
| | `--src-port-end <port>` | Last source UDP port, inclusive (default: 1243) |
| | `--retry-timeout <us>` | Per-retry timeout in microseconds (default: 1000) |
| | `--max-retries <n>` | Max retransmit attempts per packet (default: 3) |
| | `--pending-scale <n>` | Pending table size = next_pow2(max_in_flight × n) (default: 2) |
| | `--num-keys <n>` | Total keys in server hashtable; must match server's `--num-keys` |
| | `--warmup <seconds>` | Discard latency samples during initial warmup window (default: 2) |
| `-h` | `--help` | Print usage |

The source port range is partitioned evenly across queues, with one `rte_flow`
rule per port steering responses back to the originating queue.

### HT Workload Options (`-T ht`) ###

| Long | Default | Description |
|------|---------|-------------|
| `--workload <A\|B\|C>` | `B` | YCSB read/write mix (see table below) |
| `--key-dist <uniform\|zipf>` | `uniform` | Key selection distribution |
| `--zipf-theta <θ>` | `0.99` | Zipf skew exponent in (0, 1); higher = hotter hot keys |

**YCSB workloads:**

| Workload | GET | SET (Update) |
|----------|-----|-------------|
| A        | 50% | 50%         |
| B        | 95% | 5%          |
| C        | 100%| 0%          |

Workload B matches the prior hard-coded behaviour and is the default when
`--workload` is omitted.

**Key distributions:**
- `uniform` — keys drawn uniformly at random over `[0, num-keys)`.
- `zipf` — keys drawn from a Zipf distribution. The normalization constant
  `ζ(n, θ)` is computed once at startup (O(n)); per-packet sampling is O(1).
  At the YCSB default of θ = 0.99, the top 1% of keys receive ~67% of traffic.

### Examples ###

Fixed load — 100 Kpps for 10 seconds:
```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T get -b 1 -n 1 -m fixed -r 100000 \
  --retry-timeout 1000 --max-retries 3
```

Fixed load with per-packet stats saved to file:
```
sudo ./client/build/client -l 2 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T set -b 32 -n 1 \
  -m fixed -r 1000000 -o per_packet_stats.csv \
  --retry-timeout 1000 --max-retries 3
```

Variable load — ramp from 1 Mpps to 3 Mpps in 500 Kpps steps every 5 seconds:
```
sudo ./client/build/client -l 2 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T set -b 32 -n 1 \
  -m variable -r 1000000 -e 3000000 -k 500000 -i 5 -o per_packet_stats.csv \
  --retry-timeout 1000 --max-retries 3
```

Ladder load — same as variable but ramps back down after reaching the peak:
```
sudo ./client/build/client -l 2 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T set -b 32 -n 1 \
  -m ladder -r 1000000 -e 3000000 -k 500000 -i 5 -o per_packet_stats.csv \
  --retry-timeout 1000 --max-retries 3
```

Custom source port range across 2 queues (must divide evenly):
```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T get -b 1 -n 1 -m fixed -r 1000000 \
  --src-port-start 1024 --src-port-end 2047 \
  --retry-timeout 1000 --max-retries 3
```

Custom pending table size — increase `--pending-scale` to reduce slot collisions at high retry rates:
```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T get -b 1 -n 1 -m fixed -r 500000 \
  --retry-timeout 500 --max-retries 2 --pending-scale 4
```

YCSB-A workload (50/50 read/write) with uniform key distribution:
```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T ht -b 32 -n 4 -m fixed -r 3000000 \
  --src-port-start 1024 --src-port-end 1031 --workload A
```

YCSB-B workload (95/5 read/write) with Zipf key distribution (θ = 0.99):
```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 10 -T ht -b 32 -n 4 -m fixed -r 3000000 \
  --src-port-start 1024 --src-port-end 1031 --workload B --key-dist zipf --zipf-theta 0.99
```

YCSB-C (read-only) with Zipf, sweeping load from 1 Mpps to 10 Mpps:
```
sudo ./client/build/client -l 1,3 --socket-mem=128 -- \
  -s 192.168.1.2 -M b8:3f:d2:18:ad:57 -p 10002 -t 30 -T ht -b 32 -n 4 -m ladder \
  -r 1000000 -e 10000000 -k 1000000 -i 2 \
  --src-port-start 1024 --src-port-end 1031 --workload C --key-dist zipf
```

## Run Server

```
sudo ./server/build/server [EAL args] -- -T <interval> -q <queues>
```

| Option | Description |
|--------|-------------|
| `-T <seconds>` | Stats print interval (0 = no stats) |
| `-q <n>` | Number of RX/TX queues (must match lcore count) |

Example — 6 queues, print stats every second:
```
sudo ./server/build/server -l 0,1,2,3,4,5 -- -T 1 -q 6
```

## Thread Scaling Experiment

`scripts/exp_thread_scaling.sh` measures server throughput as a function of
thread count.  For each value of `-n` (1 … MAX\_THREADS) it:

1. Starts the MICA\_MULTI server on `node-0` via SSH.
2. Pushes the DPDK meminfo files to the DPU.
3. Starts the DPU port monitor.
4. Sweeps offered load from `PPS_START` up in `PPS_STEP` increments until the
   measured miss rate exceeds `MAX_MISS_PCT`, recording every data point.

Results are written to a timestamped CSV under `scripts/results/`.

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `--max-threads N` | `8` | Highest thread count to test |
| `--max-miss-pct N` | `5` | Stop load sweep when miss% ≥ N |
| `--pps-start N` | `500000` | First offered load (pps) |
| `--pps-step N` | `500000` | Load increment per step |
| `--pps-max N` | `10000000` | Hard cap on offered load |
| `--bench-time N` | `10` | Client run duration per step (seconds) |
| `--server-wait N` | `10` | Seconds to wait after server start |
| `--cooldown N` | `5` | Seconds between thread-count iterations |
| `--result-dir DIR` | `results/thread_scaling_<ts>` | Output directory |
| `--result-file FILE` | _(new file per run)_ | Append to an existing CSV (skips header) |
| `--num-keys N` | _(client default)_ | Key population size; must match the server's `--num-keys` |
| `--workload W` | `B` | YCSB workload: `A` (50r/50w), `B` (95r/5w), `C` (100r) |
| `--key-dist D` | `uniform` | Key distribution: `uniform` or `zipf` |
| `--zipf-theta T` | `0.99` | Zipf skew exponent in (0, 1); only used with `--key-dist zipf` |

### CSV columns

```
threads, offered_pps, sent_mpps, recv_mpps, missing_mpps, miss_pct,
lat_mean_us, lat_p50_us, lat_p99_us, retries, retries_exhausted,
workload, key_dist, zipf_theta, num_keys
```

### Examples

Default run — threads 1–8, 5% miss threshold:
```
sudo ./scripts/exp_thread_scaling.sh
```

Test up to 4 threads with a tighter 2% miss threshold and finer 250 Kpps steps:
```
sudo ./scripts/exp_thread_scaling.sh --max-threads 4 --max-miss-pct 2 --pps-start 500000 --pps-step 250000 --pps-max 8000000
```

YCSB-A with Zipf key distribution (θ = 0.99) and 1 M key population:
```
sudo ./scripts/exp_thread_scaling.sh --workload A --key-dist zipf --zipf-theta 0.99 --num-keys 1000000
```

Save results to a custom directory:
```
sudo ./scripts/exp_thread_scaling.sh --result-dir /tmp/scaling_$(date +%s)
```

## Plot

### Setup ###

```
pip install pandas matplotlib
sudo apt install msttcorefonts -qq
rm ~/.cache/matplotlib -rf
```

### Plot Data ###

```
./scripts/plot_packets_data.py -c confs/plot_workload.conf -f per_packet_stats.csv
```

### Thread Scaling Plot ###

Generates one throughput-vs-threads graph per (YCSB workload × key distribution) pair.
Each graph shows the peak DPU and Host throughput (Mpps) achieved at each thread count,
where peak is the highest `recv_mpps` at ≤ 5% miss rate.

```
./scripts/plot_thread_scaling.py -f scripts/results/<dir>/results.csv
```

| Option | Description |
|--------|-------------|
| `-f`, `--csv-file` | Path to the results CSV file (required) |
| `--outback` | Path to Outback throughput CSV with a `throughput_ops_per_sec` column; draws a horizontal reference line |
| `--dpu` | Path to a DPU-only results CSV (same format as `-f`); all rows are treated as thread count 0 and plotted as the x=0 baseline |

Output files are written to the same directory as the CSV, named
`<csv_name>_scaling_<workload>_<dist>.{pdf,png}` (e.g. `results_scaling_b_zipf.pdf`).

Example — plot results from a specific run:
```
./scripts/plot_thread_scaling.py -f scripts/results/thread_scaling_20260429_161630/results.csv
```

Example — include Outback baseline:
```
./scripts/plot_thread_scaling.py -f scripts/results/thread_scaling_20260429_161630/results.csv \
  --outback scripts/results/outback_throughput.csv
```

Example — include DPU-only baseline at x=0 and Outback reference line:
```
./scripts/plot_thread_scaling.py -f scripts/results/thread_scaling_20260429_161630/results.csv \
  --outback scripts/results/outback_throughput.csv \
  --dpu scripts/results/thread_scaling_dpu_only/results.csv
```
