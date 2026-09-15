#!/usr/bin/env python3

##########################
# Plot Thread Scaling    #
##########################

import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib import rcParams
import argparse

# PLOT_WIDTH = 3.38
PLOT_WIDTH = 2.5
PLOT_HEIGHT = 1.5

DPU_COLOR      = '#e41a1c'  # red
HOST_COLOR     = '#377eb8'  # blue
COMBINED_COLOR = '#ff7f00'  # orange
OUTBACK_COLOR  = '#984ea3'  # purple
ERPC_COLOR     = '#4daf4a'  # green

SMALL_SIZE      = 8
ULTRA_SMALL_SIZE = 6

plt.rc('font',   size=SMALL_SIZE)
plt.rc('axes',   titlesize=SMALL_SIZE)
plt.rc('axes',   labelsize=SMALL_SIZE)
plt.rc('xtick',  labelsize=SMALL_SIZE)
plt.rc('ytick',  labelsize=SMALL_SIZE)

LINE_WIDTH = 0.5

rcParams['axes.spines.right'] = False
rcParams['axes.spines.top']   = False
rcParams['legend.fontsize']   = ULTRA_SMALL_SIZE

MISS_PCT_THRESHOLD = 5.0  # consider rows below this miss rate as valid operating points

parser = argparse.ArgumentParser(description='Plot thread-scaling graphs from experiment CSV')
parser.add_argument('--dpu-host', type=str, required=True,
                    help='Path to the results CSV file')
parser.add_argument('--outback', type=str, default=None,
                    help='Path to Outback throughput CSV (throughput_ops_per_sec column)')
parser.add_argument('--dpu', type=str, default=None,
                    help='Path to DPU-only CSV (same format as -f); threads column is set to 0 and prepended')
parser.add_argument('--erpc', type=str, default=None,
                    help='Path to eRPC results CSV (threads,ycsb,skew,throughput_mrps columns)')
args = parser.parse_args()

CSV_FILE_PATH = os.path.abspath(args.dpu_host)
DIR_NAME      = os.path.dirname(CSV_FILE_PATH)
CSV_FILE_NAME = os.path.splitext(os.path.basename(CSV_FILE_PATH))[0]

OUTBACK_MPPS = None
if args.outback:
    ob = pd.read_csv(os.path.abspath(args.outback))
    OUTBACK_MPPS = ob['throughput_ops_per_sec'].max() / 1e6

ERPC_DF = None
if args.erpc:
    ERPC_DF = pd.read_csv(os.path.abspath(args.erpc))


def _cm_reorder(handles, labels, ncol):
    """Reorder items from reading order to matplotlib's column-major storage order."""
    n = len(handles)
    nrows = (n + ncol - 1) // ncol
    h_out, l_out = [None] * n, [None] * n
    for i, (h, l) in enumerate(zip(handles, labels)):
        row, col = divmod(i, ncol)
        j = col * nrows + row
        h_out[j], l_out[j] = h, l
    return h_out, l_out


def peak_per_thread(group):
    """Return the row with the highest recv_mpps among valid (low miss) operating points."""
    valid = group[group['miss_pct'] < MISS_PCT_THRESHOLD]
    if valid.empty:
        valid = group  # fall back to all rows if none pass the threshold
    return valid.loc[valid['recv_mpps'].idxmax()]


def plot_scaling(df, workload, key_dist):
    subset = df[(df['workload'] == workload) & (df['key_dist'] == key_dist)]
    if subset.empty:
        return

    peak = (subset.groupby('threads', group_keys=False)
                  .apply(peak_per_thread)
                  .reset_index(drop=True)
                  .sort_values('threads'))

    label_dist = 'Zipf' if key_dist == 'zipf' else 'Uniform'
    label_wl   = f'YCSB-{workload}'
    tag        = f'{workload.lower()}_{key_dist}'

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    peak = peak.copy()
    peak['combined_mpps'] = peak['dpu_mpps'] + peak['host_mpps']

    dpu_line, = ax.plot(peak['threads'], peak['dpu_mpps'],
                        marker='o', markersize=3, color=DPU_COLOR,
                        linewidth=LINE_WIDTH, label='DPU')
    host_line, = ax.plot(peak['threads'], peak['host_mpps'],
                         marker='x', markersize=3, color=HOST_COLOR,
                         linewidth=LINE_WIDTH, label='Host')
    combined_line, = ax.plot(peak['threads'], peak['combined_mpps'],
                             marker='d', markersize=3, color=COMBINED_COLOR,
                             linewidth=LINE_WIDTH, label='Combined')

    legend_handles = [dpu_line, host_line, combined_line]
    legend_labels  = ['NIC', 'Host', 'Combined']

    if OUTBACK_MPPS is not None:
        outback_line = ax.axhline(y=OUTBACK_MPPS, color=OUTBACK_COLOR, linestyle='--',
                                  linewidth=LINE_WIDTH, label='Outback')
        legend_handles.append(outback_line)
        legend_labels.append('Outback')

    erpc_vals = None
    if ERPC_DF is not None:
        erpc_sub = ERPC_DF[(ERPC_DF['ycsb'] == workload.lower()) &
                           (ERPC_DF['skew'] == key_dist)].sort_values('threads')
        if not erpc_sub.empty:
            erpc_line, = ax.plot(erpc_sub['threads'], erpc_sub['throughput_mrps'],
                                 marker='s', markersize=3, color=ERPC_COLOR,
                                 linewidth=LINE_WIDTH, linestyle='--', label='eRPC')
            legend_handles.append(erpc_line)
            legend_labels.append('eRPC')
            erpc_vals = erpc_sub['throughput_mrps'].values

    ax.set_xlabel('Host CPU Cores')
    ax.set_ylabel('Throughput (Mpps)')

    threads = sorted(peak['threads'].unique())
    ax.set_xticks(threads)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f'{int(x)}'))

    ax.set_xlim(left=0, right=max(threads) + 0.5)
    y_max = peak[['dpu_mpps', 'host_mpps', 'combined_mpps']].values.max()
    if erpc_vals is not None:
        y_max = max(y_max, erpc_vals.max())
    ax.set_ylim(bottom=0, top=y_max * 1.2)

    n_leg = len(legend_handles)
    if n_leg <= 4:
        leg_h, leg_l, ncol_leg = legend_handles, legend_labels, n_leg
    else:
        ncol_leg = 3
        leg_h, leg_l = _cm_reorder(legend_handles, legend_labels, ncol_leg)

    ax.legend(leg_h, leg_l,
              loc='lower center', bbox_to_anchor=(0.5, 1.0),
              ncol=ncol_leg, frameon=False)

    fig.tight_layout()

    for ext in ('pdf', 'png'):
        path = f'{DIR_NAME}/{CSV_FILE_NAME}_scaling_{tag}.{ext}'
        fig.savefig(path, dpi=300)
        print(f'Saved: {path}')

    plt.close(fig)


def main():
    df = pd.read_csv(CSV_FILE_PATH)

    if args.dpu:
        dpu_df = pd.read_csv(os.path.abspath(args.dpu))
        dpu_df['threads'] = 0
        df = pd.concat([dpu_df, df], ignore_index=True)

    pairs = [
        ('B', 'zipf'),
        ('B', 'uniform'),
        ('A', 'zipf'),
        ('A', 'uniform'),
        ('C', 'zipf'),
        ('C', 'uniform'),
    ]

    for workload, key_dist in pairs:
        plot_scaling(df, workload, key_dist)


if __name__ == '__main__':
    main()
