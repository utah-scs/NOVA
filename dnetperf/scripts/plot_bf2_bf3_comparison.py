#!/usr/bin/env python3

##############################
# Plot BF2 vs BF3 Comparison #
##############################

import os
import pandas as pd
import matplotlib.pyplot as plt
from matplotlib import rcParams
import numpy as np
import argparse

PLOT_WIDTH = 2.5
PLOT_HEIGHT = 1.5

BF2_COLOR = '#e41a1c'  # red
BF3_COLOR = '#377eb8'  # blue

SMALL_SIZE = 8
ULTRA_SMALL_SIZE = 6

plt.rc('font',  size=SMALL_SIZE)
plt.rc('axes',  titlesize=SMALL_SIZE)
plt.rc('axes',  labelsize=SMALL_SIZE)
plt.rc('xtick', labelsize=SMALL_SIZE)
plt.rc('ytick', labelsize=SMALL_SIZE)

rcParams['axes.spines.right'] = False
rcParams['axes.spines.top']   = False
rcParams['legend.fontsize']   = ULTRA_SMALL_SIZE

MISS_PCT_THRESHOLD = 5.0  # consider rows below this miss rate as valid operating points

parser = argparse.ArgumentParser(description='Compare BF2 vs BF3 DPU-only throughput for YCSB-C')
parser.add_argument('--bf2', type=str,
                    default='scripts/results/thread_scaling_dpu_only/results.csv',
                    help='Path to BF2 results CSV')
parser.add_argument('--bf3', type=str,
                    default='scripts/results/thread_scaling_dpu_only2/results.csv',
                    help='Path to BF3 results CSV')
parser.add_argument('--workload', type=str, default='C',
                    help='YCSB workload letter to compare (default: C)')
parser.add_argument('--out-dir', type=str, default='scripts/results/bf2_bf3_comparison',
                    help='Directory to save output plot')
args = parser.parse_args()

BF2_PATH = os.path.abspath(args.bf2)
BF3_PATH = os.path.abspath(args.bf3)
OUT_DIR  = os.path.abspath(args.out_dir)


def peak_dpu_mpps(df, workload, key_dist):
    """Return the highest dpu_mpps among valid (low miss) operating points."""
    subset = df[(df['workload'] == workload) & (df['key_dist'] == key_dist)]
    if subset.empty:
        return 0.0
    valid = subset[subset['miss_pct'] < MISS_PCT_THRESHOLD]
    if valid.empty:
        valid = subset
    return valid['dpu_mpps'].max()


def main():
    bf2_df = pd.read_csv(BF2_PATH)
    bf3_df = pd.read_csv(BF3_PATH)

    key_dists = ['uniform', 'zipf']
    labels = ['Uniform', 'Zipf']

    bf2_vals = [peak_dpu_mpps(bf2_df, args.workload, kd) for kd in key_dists]
    bf3_vals = [peak_dpu_mpps(bf3_df, args.workload, kd) for kd in key_dists]

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    x = np.arange(len(key_dists))
    bar_width = 0.35

    bf2_bars = ax.bar(x - bar_width / 2, bf2_vals, bar_width,
                      color=BF2_COLOR, label='BF2')
    bf3_bars = ax.bar(x + bar_width / 2, bf3_vals, bar_width,
                      color=BF3_COLOR, label='BF3')

    ax.bar_label(bf2_bars, fmt='%.2f', fontsize=ULTRA_SMALL_SIZE, padding=1)
    ax.bar_label(bf3_bars, fmt='%.2f', fontsize=ULTRA_SMALL_SIZE, padding=1)

    ax.set_xlabel(f'YCSB-{args.workload} Key Distribution')
    ax.set_ylabel('Throughput (M op/s)')
    ax.set_xticks(x)
    ax.set_xticklabels(labels)

    y_max = max(bf2_vals + bf3_vals)
    ax.set_ylim(bottom=0, top=y_max * 1.3)

    ax.legend([bf2_bars, bf3_bars], ['BF2', 'BF3'],
              loc='lower center', bbox_to_anchor=(0.5, 1.0),
              ncol=2, frameon=False)

    fig.tight_layout()

    os.makedirs(OUT_DIR, exist_ok=True)
    tag = f'{args.workload.lower()}'
    for ext in ('pdf', 'png'):
        path = f'{OUT_DIR}/bf2_bf3_comparison_{tag}.{ext}'
        fig.savefig(path, dpi=300)
        print(f'Saved: {path}')

    plt.close(fig)


if __name__ == '__main__':
    main()
