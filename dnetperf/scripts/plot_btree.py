#!/usr/bin/env python3

################
# Plot B+ Tree #
################

# Script for plotting the tput/latency and tx bandwidth of a B+ tree
# lookup experiment from four separate per-benchmark CSV files (dpu,
# dpu-cache, host, rdma), each produced by run_bpt_exp.sh (or the RDMA
# equivalent) with columns: bench,<pps|threads>,avg_tput_mpps,avg_lat_99th
#
# usage: plot_btree.py --dpu <dpu.csv> --dpu-cache <dpu_cache.csv> \
#                       --host <host.csv> --rdma <rdma.csv> \
#                       [-o <out_dir>] [-n <out_name>]

import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager
import matplotlib.ticker as ticker
from matplotlib.lines import Line2D
from matplotlib import rcParams;
import configparser
import argparse

#PLOT_WIDTH = 3.38
PLOT_WIDTH = 2.5
PLOT_HEIGHT = 1.5

DPU_COLOR = '#e41a1c' # red
DPU_CACHE_COLOR = '#4daf4a'
HOST_COLOR = '#377eb8' # blue
RDMA_COLOR = '#ff7f00'
RDMA_B_COLOR = '#984ea3'

# Font settings
SMALL_SIZE = 8
ULTRA_SMALL_SIZE = 6
MEDIUM_SIZE = 10
BIGGER_SIZE = 12
plt.rc('font', size=SMALL_SIZE)          # controls default text sizes
plt.rc('axes', titlesize=SMALL_SIZE)     # fontsize of the axes title
plt.rc('axes', labelsize=SMALL_SIZE)    # fontsize of the x and y labels
plt.rc('xtick', labelsize=SMALL_SIZE)    # fontsize of the tick labels
plt.rc('ytick', labelsize=SMALL_SIZE)    # fontsize of the tick labels
# plt.rc('legend', fontsize=SMALL_SIZE)    # legend fontsize
# plt.rc('figure', titlesize=BIGGER_SIZE)

# plt.rc('font', family='Helvetica')
# plt.rc('font', family='Times New Roman')

LINE_WIDTH = 0.5

rcParams["axes.spines.right"] = False
rcParams["axes.spines.top"] = False
rcParams['legend.fontsize'] = ULTRA_SMALL_SIZE

parser = argparse.ArgumentParser(description='Plot Throughput, RTT, and Packet Loss graphs for B+ tree lookup')
parser.add_argument('--dpu', type=str, required=True, help='CSV file for the DPU (NIC) benchmark')
parser.add_argument('--dpu-cache', type=str, required=True, help='CSV file for the DPU cache (NIC_CACHE) benchmark')
parser.add_argument('--host', type=str, required=True, help='CSV file for the Host benchmark')
parser.add_argument('--rdma', type=str, required=True, help='CSV file for the RDMA benchmark')
parser.add_argument('-o', '--out-dir', type=str, default=None, help='Directory to write the plots to (default: directory of the --dpu csv)')
parser.add_argument('-n', '--out-name', type=str, default='bpt', help='Base file name for the generated plots')

args = parser.parse_args()

# Configuration variables
DPU_CSV_PATH = os.path.abspath(args.dpu)
DPU_CACHE_CSV_PATH = os.path.abspath(args.dpu_cache)
HOST_CSV_PATH = os.path.abspath(args.host)
RDMA_CSV_PATH = os.path.abspath(args.rdma)

DIR_NAME = os.path.abspath(args.out_dir) if args.out_dir else os.path.dirname(DPU_CSV_PATH)
OUT_NAME = args.out_name

# Constants for tx bw plot
HOST_DPU_REQ_PKT_SZ = 772 # Size of each request packets for host and dpu
RDMA_RD_SZ = 665 # Size of each RDMA read request
NUM_LEVELS = 5 # Number of levels in the B+ tree


def plot_tput_lat(dpu_df, dpu_cache_df, host_df, rdma_df):
    FIG_FILE_NAME_PDF = "{}/{}_tput_lat_small.pdf".format(DIR_NAME, OUT_NAME)
    FIG_FILE_NAME_PNG = "{}/{}_tput_lat_small.png".format(DIR_NAME, OUT_NAME)

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    dpu_df.plot(x='avg_tput_mpps', y='avg_lat_99th', ax=ax, kind='line', marker='o', markersize=2, color=DPU_COLOR, label='NIC', linewidth=LINE_WIDTH)
    host_df.plot(x='avg_tput_mpps', y='avg_lat_99th', ax=ax, kind='line', marker='x', markersize=2, color=HOST_COLOR, label='Host', linewidth=LINE_WIDTH)
    rdma_df.plot(x='avg_tput_mpps', y='avg_lat_99th', ax=ax, kind='line', marker='*', markersize=2, color=RDMA_B_COLOR, label='RDMA', linewidth=LINE_WIDTH)
    dpu_cache_df.plot(x='avg_tput_mpps', y='avg_lat_99th', ax=ax, kind='line', marker='1', markersize=2, color=DPU_CACHE_COLOR, label='NIC_CACHE', linewidth=LINE_WIDTH)

    plt.xlabel('Throughput (M op/s)')
    plt.ylabel('99th Latency (us)')

    ax.xaxis.set_ticks(np.arange(0, 4, 0.5))
    # Print x axis in integer format
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x)))
    # Print x axis in float format
    # ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x/10**6)))

    ax.set_yscale('log')
    ax.yaxis.set_ticks([10**i for i in range(0, 5)])
    ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
    # ax.yaxis.get_ticklocs(minor=True)
    # ax.minorticks_on()

    ax.set_ylim(bottom=1, top=50000)
    ax.set_xlim(left=0, right=4)

    # Remove legend frame
    plt.legend(frameon=False)

    # Remove legend if only one line is plotted
    # ax.get_legend().remove()

    # labels at the top of the plot
    plt.legend(loc='lower center', bbox_to_anchor=(0.5, 0.8), ncol=2, frameon=False)

    fig.tight_layout()
    fig.savefig(FIG_FILE_NAME_PDF, dpi=300)
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300)

def plot_tx_bw(dpu_df, dpu_cache_df, host_df, rdma_df):
    host_df['tx_bw'] = (host_df['avg_tput_mpps'] * 10**6 * HOST_DPU_REQ_PKT_SZ * 8) / 2**30  # Convert to Gbps
    dpu_df['tx_bw'] = (dpu_df['avg_tput_mpps'] * 10**6 * HOST_DPU_REQ_PKT_SZ * 8) / 2**30  # Convert to Gbps
    dpu_cache_df['tx_bw'] = (dpu_cache_df['avg_tput_mpps'] * 10**6 * HOST_DPU_REQ_PKT_SZ * 8) / 2**30  # Convert to Gbps
    rdma_df['tx_bw'] = (rdma_df['avg_tput_mpps'] * 10**6 * RDMA_RD_SZ * NUM_LEVELS * 8) / 2**30  # Convert to Gbps

    FIG_FILE_NAME_PDF = "{}/{}_tx_bw_small.pdf".format(DIR_NAME, OUT_NAME)
    FIG_FILE_NAME_PNG = "{}/{}_tx_bw_small.png".format(DIR_NAME, OUT_NAME)

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    dpu_df.plot(x='avg_tput_mpps', y='tx_bw', ax=ax, kind='line', marker='o', markersize=2, color=DPU_COLOR, label='NIC', linewidth=LINE_WIDTH)
    host_df.plot(x='avg_tput_mpps', y='tx_bw', ax=ax, kind='line', marker='x', markersize=2, color=HOST_COLOR, label='Host', linewidth=LINE_WIDTH)
    rdma_df.plot(x='avg_tput_mpps', y='tx_bw', ax=ax, kind='line', marker='*', markersize=2, color=RDMA_B_COLOR, label='RDMA', linewidth=LINE_WIDTH)
    dpu_cache_df.plot(x='avg_tput_mpps', y='tx_bw', ax=ax, kind='line', marker='1', markersize=2, color=DPU_CACHE_COLOR, label='NIC_CACHE', linewidth=LINE_WIDTH)

    plt.xlabel('Throughput (M op/s)')
    plt.ylabel('Bandwidth (Gbps)')

    ax.xaxis.set_ticks(np.arange(0, 4, 0.5))
    # Print x axis in integer format
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x)))
    # Print x axis in float format
    # ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x/10**6)))

    # ax.set_yscale('log')
    # ax.yaxis.set_ticks([10**i for i in range(0, 5)])
    # ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
    # ax.yaxis.get_ticklocs(minor=True)
    # ax.minorticks_on()

    ax.set_ylim(bottom=0, top=60)
    ax.set_xlim(left=0, right=4)

    # Remove legend frame
    plt.legend(frameon=False)

    # Remove legend if only one line is plotted
    # ax.get_legend().remove()

    # labels at the top of the plot
    plt.legend(loc='lower center', bbox_to_anchor=(0.5, 0.8), ncol=2, frameon=False)

    fig.tight_layout()
    fig.savefig(FIG_FILE_NAME_PDF, dpi=300)
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300)

def main():
    dpu_df = pd.read_csv(DPU_CSV_PATH)
    dpu_cache_df = pd.read_csv(DPU_CACHE_CSV_PATH)
    host_df = pd.read_csv(HOST_CSV_PATH)
    rdma_df = pd.read_csv(RDMA_CSV_PATH)

    plot_tput_lat(dpu_df, dpu_cache_df, host_df, rdma_df)
    plot_tx_bw(dpu_df, dpu_cache_df, host_df, rdma_df)

if __name__ == "__main__":
    main()
