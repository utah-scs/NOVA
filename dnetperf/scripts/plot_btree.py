#!/usr/bin/env python3

################
# Plot B+ Tree #
################

# Script for plotting the tput of packets from CSV file
# usage: plot_packets_tput.py <csv_file> <file_name_append> <plot_title> <exp_run_time> <x_axis_start> <x_axis_end> <x_axis_step>

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
    
parser = argparse.ArgumentParser(description='Plot Throughput, RTT, and Packet Loss graphs')
parser.add_argument('-f','--csv-file', type=str, help='Packets data file for the plots', required=True)

args = parser.parse_args()
data_file = args.csv_file

# Configuration variables
CSV_FILE_PATH = os.path.abspath(data_file)
DIR_NAME = os.path.dirname(CSV_FILE_PATH)
CSV_FILE = os.path.basename(CSV_FILE_PATH)
CSV_FILE_NAME = os.path.splitext(CSV_FILE)[0]

# Constants for tx bw plot
HOST_DPU_REQ_PKT_SZ = 772 # Size of each request packets for host and dpu
RDMA_RD_SZ = 665 # Size of each RDMA read request
NUM_LEVELS = 5 # Number of levels in the B+ tree


def plot_tput_lat(df):
    FIG_FILE_NAME_PDF = "{}/{}_tput_lat_small.pdf".format(DIR_NAME, CSV_FILE_NAME)
    FIG_FILE_NAME_PNG = "{}/{}_tput_lat_small.png".format(DIR_NAME, CSV_FILE_NAME)
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
    
    # Host interfercenc
    df.plot(x='dpu_tput', y='dpu_lat', ax=ax, kind='line', marker='o', markersize=2, color=DPU_COLOR, label='NIC', linewidth=LINE_WIDTH)
    df.plot(x='host_tput', y='host_lat', ax=ax, kind='line', marker='x', markersize=2, color=HOST_COLOR, label='Host', linewidth=LINE_WIDTH)
    # df.plot(x='rdma_tput', y='rdma_lat', ax=ax, kind='line', marker='d', markersize=2, color=RDMA_COLOR, label='RDMA', linewidth=LINE_WIDTH)
    df.plot(x='rdma_batch_tput', y='rdma_batch_lat', ax=ax, kind='line', marker='*', markersize=2, color=RDMA_B_COLOR, label='RDMA', linewidth=LINE_WIDTH)
    df.plot(x='dpu_cache_tput', y='dpu_cache_lat', ax=ax, kind='line', marker='1', markersize=2, color=DPU_CACHE_COLOR, label='NIC_CACHE', linewidth=LINE_WIDTH)
    
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

def plot_tx_bw(df):
    df['tx_bw_host'] = (df['host_tput'] * 10**6 * HOST_DPU_REQ_PKT_SZ * 8) / 2**30  # Convert to Gbps
    df['tx_bw_dpu'] = (df['dpu_tput'] * 10**6 * HOST_DPU_REQ_PKT_SZ * 8) / 2**30  # Convert to Gbps
    df['tx_bw_dpu_cache'] = (df['dpu_cache_tput'] * 10**6 * HOST_DPU_REQ_PKT_SZ * 8) / 2**30  # Convert to Gbps
    df['tx_bw_rdma'] = (df['rdma_tput'] * 10**6 * RDMA_RD_SZ * NUM_LEVELS * 8) / 2**30  # Convert to Gbps
    df['tx_bw_rdma_batch'] = (df['rdma_batch_tput'] * 10**6 * RDMA_RD_SZ * NUM_LEVELS * 8) / 2**30  # Convert to Gbps
    
    FIG_FILE_NAME_PDF = "{}/{}_tx_bw_small.pdf".format(DIR_NAME, CSV_FILE_NAME)
    FIG_FILE_NAME_PNG = "{}/{}_tx_bw_small.png".format(DIR_NAME, CSV_FILE_NAME)
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
    
    # Host interfercenc
    df.plot(x='dpu_tput', y='tx_bw_dpu', ax=ax, kind='line', marker='o', markersize=2, color=DPU_COLOR, label='NIC', linewidth=LINE_WIDTH)
    df.plot(x='host_tput', y='tx_bw_host', ax=ax, kind='line', marker='x', markersize=2, color=HOST_COLOR, label='Host', linewidth=LINE_WIDTH)
    # df.plot(x='rdma_tput', y='tx_bw_rdma', ax=ax, kind='line', marker='d', markersize=2, color=RDMA_COLOR, label='RDMA', linewidth=LINE_WIDTH)
    df.plot(x='rdma_batch_tput', y='tx_bw_rdma_batch', ax=ax, kind='line', marker='*', markersize=2, color=RDMA_B_COLOR, label='RDMA', linewidth=LINE_WIDTH)
    df.plot(x='dpu_cache_tput', y='tx_bw_dpu_cache', ax=ax, kind='line', marker='1', markersize=2, color=DPU_CACHE_COLOR, label='NIC_CACHE', linewidth=LINE_WIDTH)
    
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
    # Read the CSV file
    df = pd.read_csv(CSV_FILE_PATH)
    plot_tput_lat(df)
    plot_tx_bw(df)

if __name__ == "__main__":
    main()
