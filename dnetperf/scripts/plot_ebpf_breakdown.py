#!/usr/bin/env python3

# Script for plotting the end-to-end cost of running MICA as a native BESS
# module versus through NOVA's eBPF path (mica-module vs. mica-nova).
# usage: plot_ebpf_breakdown.py -d <data_dir> [-o <output_dir>]

import os
import argparse
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.lines import Line2D
from matplotlib import rcParams

PLOT_WIDTH = 3.38
PLOT_HEIGHT = 2

MODULE_COLOR = '#377eb8'  # blue
NOVA_COLOR   = '#e41a1c'  # red

LINE_WIDTH  = 0.75
MARKER_SIZE = 4

BAR_PLOT_WIDTH  = 2.5
BAR_PLOT_HEIGHT = 1.5

# Font sizes match plot_thread_scaling_facets.py
TITLE_SIZE = 5     # axis titles, legend text
TEXT_SIZE = 4.5    # tick labels, bar labels
plt.rc('font',   size=TEXT_SIZE)
plt.rc('axes',   titlesize=TITLE_SIZE)
plt.rc('axes',   labelsize=TITLE_SIZE)
plt.rc('xtick',  labelsize=TEXT_SIZE)
plt.rc('ytick',  labelsize=TEXT_SIZE)
plt.rc('legend', fontsize=TITLE_SIZE)
plt.rc('font',   family='Helvetica')

rcParams["axes.spines.right"] = False
rcParams["axes.spines.top"]   = False

# Column names in the CSV files
COLUMN_DIST   = 'dist'
COLUMN_PPS    = 'pps'
COLUMN_TPUT   = 'avg_tput_mpps'
COLUMN_LAT99  = 'avg_lat_99th'

# A benchmark is considered saturated once its 99th latency crosses this
# many microseconds and stays above it -- points at/after that offered
# load are overload artifacts (client retries piling up), not the
# steady-state cost of the configuration, so they are dropped before plotting.
SATURATION_LATENCY_US = 500
SATURATION_PERSIST     = 2

parser = argparse.ArgumentParser(description='Plot end-to-end MICA latency/throughput: BESS module vs. NOVA eBPF')
parser.add_argument('-d', '--data-dir', type=str, default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'dnetperf', 'scripts', 'results', 'end-to-end-ebpf'),
                     help='Directory containing mica-module.csv and mica-nova.csv')
parser.add_argument('-o', '--output-dir', type=str, default=None,
                     help='Directory to write the figure to (default: same as data-dir)')
parser.add_argument('-n', '--output-name', type=str, default='ebpf_breakdown',
                     help='Output file name prefix (without extension)')
parser.add_argument('-l', '--latency-slo', type=float, default=200,
                     help='99th latency target (us) for the throughput bar plot (default: 200)')

args = parser.parse_args()

DATA_DIR = os.path.abspath(args.data_dir)
OUTPUT_DIR = os.path.abspath(args.output_dir) if args.output_dir else DATA_DIR

# Key distributions to plot, and their display names. run_ht_exp.sh records
# the distribution in the 'dist' column; CSVs from before that column existed
# were all run with the uniform distribution.
DISTS = [('uniform', 'Uniform'), ('zipf', 'Zipf')]
DEFAULT_DIST = 'uniform'

def load_series(file_name, dist):
    df = pd.read_csv(os.path.join(DATA_DIR, file_name))
    if COLUMN_DIST not in df.columns:
        df[COLUMN_DIST] = DEFAULT_DIST
    df = df[df[COLUMN_DIST] == dist]
    df = df.sort_values(COLUMN_PPS).reset_index(drop=True)

    # Drop offered loads at/after the benchmark saturates (see
    # SATURATION_LATENCY_US above).
    lat = df[COLUMN_LAT99].to_numpy()
    cutoff = len(lat)
    for i in range(len(lat)):
        if lat[i] > SATURATION_LATENCY_US:
            window = lat[i:i + SATURATION_PERSIST]
            if np.all(window > SATURATION_LATENCY_US):
                cutoff = i
                break
    return df.iloc[:cutoff]

def plot_ebpf_breakdown(dist, df_module, df_nova):
    FIG_FILE_NAME_PDF = os.path.join(OUTPUT_DIR, '{}_{}.pdf'.format(args.output_name, dist))
    FIG_FILE_NAME_PNG = os.path.join(OUTPUT_DIR, '{}_{}.png'.format(args.output_name, dist))

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    ax.plot(df_module[COLUMN_TPUT], df_module[COLUMN_LAT99],
            color=MODULE_COLOR, marker='o', markersize=MARKER_SIZE, markerfacecolor='none',
            linewidth=LINE_WIDTH, linestyle='-', label='MICA module')
    ax.plot(df_nova[COLUMN_TPUT], df_nova[COLUMN_LAT99],
            color=NOVA_COLOR, marker='x', markersize=MARKER_SIZE,
            linewidth=LINE_WIDTH, linestyle='--', label='MICA NOVA (eBPF)')

    plt.xlabel('Throughput (Mop/s)')
    plt.ylabel('99th latency (us)')

    x_top = 4
    ax.xaxis.set_ticks(np.arange(0, x_top + 0.5, 0.5))
    ax.set_xlim(left=0, right=x_top)

    y_max = pd.concat([df_module[COLUMN_LAT99], df_nova[COLUMN_LAT99]]).max()
    y_top = 10 ** np.ceil(np.log10(y_max))
    ax.set_yscale('log')
    ax.yaxis.set_major_locator(ticker.LogLocator(base=10.0, numticks=15))
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, pos: '{:g}'.format(y)))
    ax.set_ylim(bottom=10, top=y_top)

    line_module = Line2D([0], [0], color=MODULE_COLOR, marker='o', markersize=MARKER_SIZE, markerfacecolor='none',
                          linewidth=LINE_WIDTH, linestyle='-', label='MICA module')
    line_nova   = Line2D([0], [0], color=NOVA_COLOR, marker='x', markersize=MARKER_SIZE,
                          linewidth=LINE_WIDTH, linestyle='--', label='MICA NOVA (eBPF)')
    plt.legend(handles=[line_module, line_nova],
               bbox_to_anchor=(0., 1.02, 1., .102), loc="lower left",
               ncols=2, mode="expand", borderaxespad=0., frameon=False)

    fig.savefig(FIG_FILE_NAME_PDF, dpi=300, bbox_inches='tight')
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300, bbox_inches='tight')
    plt.close(fig)

def tput_at_latency(df, latency_us):
    # Highest measured throughput whose 99th latency stays below
    # latency_us, i.e. the best operating point that meets the SLO.
    valid = df[df[COLUMN_LAT99] < latency_us]
    if valid.empty:
        return 0.0
    return valid[COLUMN_TPUT].max()

def plot_ebpf_tput_bar(series):
    slo = args.latency_slo
    tag = '{:g}us'.format(slo)
    FIG_FILE_NAME_PDF = os.path.join(OUTPUT_DIR, '{}_tput_{}.pdf'.format(args.output_name, tag))
    FIG_FILE_NAME_PNG = os.path.join(OUTPUT_DIR, '{}_tput_{}.png'.format(args.output_name, tag))

    labels      = [label for _, label, _, _ in series]
    module_vals = [tput_at_latency(df_module, slo) for _, _, df_module, _ in series]
    nova_vals   = [tput_at_latency(df_nova, slo) for _, _, _, df_nova in series]
    for label, module_val, nova_val in zip(labels, module_vals, nova_vals):
        print('  {}: throughput at 99th < {:g} us: module={:.2f} Mop/s, NOVA={:.2f} Mop/s'.format(
              label, slo, module_val, nova_val))

    fig, ax = plt.subplots(figsize=(BAR_PLOT_WIDTH, BAR_PLOT_HEIGHT))

    x = np.arange(len(labels))
    bar_width = 0.35

    module_bars = ax.bar(x - bar_width / 2, module_vals, bar_width,
                         color=MODULE_COLOR, label='MICA module')
    nova_bars   = ax.bar(x + bar_width / 2, nova_vals, bar_width,
                         color=NOVA_COLOR, label='MICA NOVA (eBPF)')

    ax.bar_label(module_bars, fmt='%.2f', fontsize=TEXT_SIZE, padding=1)
    ax.bar_label(nova_bars, fmt='%.2f', fontsize=TEXT_SIZE, padding=1)

    ax.set_xlabel('Key Distribution')
    ax.set_ylabel('Throughput (Mop/s)')
    ax.set_xticks(x)
    ax.set_xticklabels(labels)

    y_max = max(module_vals + nova_vals)
    ax.set_ylim(bottom=0, top=y_max * 1.3 if y_max > 0 else 1)

    ax.legend([module_bars, nova_bars], ['MICA module', 'MICA NOVA (eBPF)'],
              loc='lower center', bbox_to_anchor=(0.5, 1.0),
              ncol=2, frameon=False)

    fig.tight_layout()
    fig.savefig(FIG_FILE_NAME_PDF, dpi=300, bbox_inches='tight')
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300, bbox_inches='tight')
    plt.close(fig)

def main():
    # (dist, label, module series, NOVA series) for each distribution that
    # has data for both benchmarks
    series = []
    for dist, label in DISTS:
        df_module = load_series('mica-module.csv', dist)
        df_nova   = load_series('mica-nova.csv', dist)
        if df_module.empty or df_nova.empty:
            print("Skipping {}: no data for {}".format(dist, ' and '.join(
                  name for name, df in (('mica-module', df_module), ('mica-nova', df_nova)) if df.empty)))
            continue
        series.append((dist, label, df_module, df_nova))

    if not series:
        print("No distribution has data for both mica-module and mica-nova, nothing to plot")
        return

    for dist, label, df_module, df_nova in series:
        print("Plotting eBPF Breakdown (MICA module vs. NOVA), {}".format(label))
        plot_ebpf_breakdown(dist, df_module, df_nova)
    print("Plotting eBPF throughput at latency SLO (MICA module vs. NOVA)")
    plot_ebpf_tput_bar(series)

if __name__ == "__main__":
    main()
