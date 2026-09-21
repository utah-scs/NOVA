#!/usr/bin/env python3

# Script for plotting the cost of running MICA lookups from different
# locations (client, host, host+NIC), reproducing Figure 8 of the paper.
# usage: plot_function_placement.py -d <data_dir> [-o <output_dir>]

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

CLIENT_COLOR   = '#e41a1c'  # red
HOST_COLOR     = '#377eb8'  # blue
COMBINED_COLOR = '#984ea3'  # purple

LINE_WIDTH  = 0.75
MARKER_SIZE = 4

SMALL_SIZE  = 8
BIGGER_SIZE = 12
plt.rc('font',   size=SMALL_SIZE)
plt.rc('axes',   titlesize=SMALL_SIZE)
plt.rc('axes',   labelsize=SMALL_SIZE)
plt.rc('xtick',  labelsize=SMALL_SIZE)
plt.rc('ytick',  labelsize=SMALL_SIZE)
plt.rc('legend', fontsize=SMALL_SIZE)
plt.rc('figure', titlesize=BIGGER_SIZE)
plt.rc('font',   family='Helvetica')

rcParams["axes.spines.right"] = False
rcParams["axes.spines.top"]   = False

# Column names in the CSV files
COLUMN_PPS    = 'pps'
COLUMN_TPUT   = 'avg_tput_mpps'
COLUMN_LAT99  = 'avg_lat_99th'

# A benchmark is considered saturated once its 99th latency crosses this
# many microseconds and stays above it -- points at/after that offered
# load are overload artifacts (client retries piling up), not the
# steady-state cost of the placement, so they are dropped before plotting.
SATURATION_LATENCY_US = 500
SATURATION_PERSIST     = 2

parser = argparse.ArgumentParser(description='Plot Figure 8: cost of running MICA lookups from different locations')
parser.add_argument('-d', '--data-dir', type=str, default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'ae', 'figure', 'fig-8'),
                     help='Directory containing client.csv, host.csv, and host_and_dpu.csv')
parser.add_argument('-o', '--output-dir', type=str, default=None,
                     help='Directory to write the figure to (default: same as data-dir)')
parser.add_argument('-n', '--output-name', type=str, default='fig8_function_placement',
                     help='Output file name (without extension)')

args = parser.parse_args()

DATA_DIR = os.path.abspath(args.data_dir)
OUTPUT_DIR = os.path.abspath(args.output_dir) if args.output_dir else DATA_DIR

def load_series(file_name):
    df = pd.read_csv(os.path.join(DATA_DIR, file_name))
    df = df.sort_values(COLUMN_PPS).reset_index(drop=True)

    # Drop offered loads at/after the benchmark saturates (see
    # SATURATION_LATENCY_US above): once latency crosses the threshold and
    # stays there, the server is overloaded and further points just show
    # collapse, not the placement's steady-state cost.
    lat = df[COLUMN_LAT99].to_numpy()
    cutoff = len(lat)
    for i in range(len(lat)):
        if lat[i] > SATURATION_LATENCY_US:
            window = lat[i:i + SATURATION_PERSIST]
            if np.all(window > SATURATION_LATENCY_US):
                cutoff = i
                break
    return df.iloc[:cutoff]

def plot_function_placement():
    FIG_FILE_NAME_PDF = os.path.join(OUTPUT_DIR, '{}.pdf'.format(args.output_name))
    FIG_FILE_NAME_PNG = os.path.join(OUTPUT_DIR, '{}.png'.format(args.output_name))

    df_client   = load_series('client.csv')
    df_host     = load_series('host.csv')
    df_combined = load_series('host_and_dpu.csv')

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    ax.plot(df_client[COLUMN_TPUT], df_client[COLUMN_LAT99],
            color=CLIENT_COLOR, marker='x', markersize=MARKER_SIZE,
            linewidth=LINE_WIDTH, linestyle='--', label='Client')
    ax.plot(df_host[COLUMN_TPUT], df_host[COLUMN_LAT99],
            color=HOST_COLOR, marker='o', markersize=MARKER_SIZE, markerfacecolor='none',
            linewidth=LINE_WIDTH, linestyle='-', label='Host')
    ax.plot(df_combined[COLUMN_TPUT], df_combined[COLUMN_LAT99],
            color=COMBINED_COLOR, marker='v', markersize=MARKER_SIZE,
            linewidth=LINE_WIDTH, linestyle='-', label='Host + NIC')

    plt.xlabel('Throughput (Mop/s)')
    plt.ylabel('99th latency (us)')

    x_max = max(df_client[COLUMN_TPUT].max(), df_host[COLUMN_TPUT].max(), df_combined[COLUMN_TPUT].max())
    x_top = np.ceil(x_max * 2) / 2  # round up to the nearest 0.5
    ax.xaxis.set_ticks(np.arange(0, x_top + 0.5, 0.5))
    ax.set_xlim(left=0, right=x_top)

    y_max = max(df_client[COLUMN_LAT99].max(), df_host[COLUMN_LAT99].max(), df_combined[COLUMN_LAT99].max())
    y_top = 10 ** np.ceil(np.log10(y_max))
    ax.set_yscale('log')
    ax.yaxis.set_major_locator(ticker.LogLocator(base=10.0, numticks=15))
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda y, pos: '{:g}'.format(y)))
    ax.set_ylim(bottom=1, top=y_top)

    line_client = Line2D([0], [0], color=CLIENT_COLOR, marker='x', markersize=MARKER_SIZE,
                          linewidth=LINE_WIDTH, linestyle='--', label='Client')
    line_host   = Line2D([0], [0], color=HOST_COLOR, marker='o', markersize=MARKER_SIZE, markerfacecolor='none',
                          linewidth=LINE_WIDTH, linestyle='-', label='Host')
    line_comb   = Line2D([0], [0], color=COMBINED_COLOR, marker='v', markersize=MARKER_SIZE,
                          linewidth=LINE_WIDTH, linestyle='-', label='Host + NIC')
    plt.legend(handles=[line_client, line_host, line_comb],
               bbox_to_anchor=(0., 1.02, 1., .102), loc="lower left",
               ncols=3, mode="expand", borderaxespad=0., frameon=False)

    fig.savefig(FIG_FILE_NAME_PDF, dpi=300, bbox_inches='tight')
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300, bbox_inches='tight')

def main():
    print("Plotting Function Placement (Figure 8)")
    plot_function_placement()

if __name__ == "__main__":
    main()
