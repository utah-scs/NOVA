#!/usr/bin/env python3

import os
import argparse
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO

HOST_DATA= 'host_data.csv'
DPU_DATA = 'dpu_data.csv'

TITLE_APPEND = ''

FONT = font_manager.FontProperties(family='Times New Roman', size=10)

X_DATA_MIN = 0
X_DATA_MAX = 220
X_AXIS_STEP = 10

def plot_rx_miss(df, m_type):
    title = 'RX Missed Packets ({}){}'.format(m_type, TITLE_APPEND)
    fig_file_name_png = 'rx_miss_{}.png'.format(m_type)
    fig_file_name_pdf = 'rx_miss_{}.pdf'.format(m_type)
    
    # create path from file name
    fig_file_name_png = os.path.join(plot_dir, fig_file_name_png)
    fig_file_name_pdf = os.path.join(plot_dir, fig_file_name_pdf)
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
    
    df.plot(kind = 'line', ax=ax, x = 'time', y = 'rx_miss', marker='x', color='b', label='RX Missed')
    
    plt.xlabel('Time', fontproperties=FONT)
    plt.ylabel('Packets Missed', fontproperties=FONT)
    ax.set_title(title, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))
    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
    ax.set_ylim(bottom=0, top=8000)
    ax.set_xlim(left=0)

    fig = plt.gcf()
    fig.savefig(fig_file_name_png, dpi=300)
    fig.savefig(fig_file_name_pdf, dpi=300)

def plot_tx_miss(df, m_type):
    title = 'TX Missed Packets ({}){}'.format(m_type, TITLE_APPEND)
    fig_file_name_png = 'tx_miss_{}.png'.format(m_type)
    fig_file_name_pdf = 'tx_miss_{}.pdf'.format(m_type)
    
    # create path from file name
    fig_file_name_png = os.path.join(plot_dir, fig_file_name_png)
    fig_file_name_pdf = os.path.join(plot_dir, fig_file_name_pdf)
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
    
    df.plot(kind = 'line', ax=ax, x = 'time', y = 'tx_miss', marker='x', color='r', label='TX Missed')
    
    plt.xlabel('Time', fontproperties=FONT)
    plt.ylabel('Packets Missed', fontproperties=FONT)
    ax.set_title(title, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))
    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
    ax.set_ylim(bottom=0, top=8000)
    ax.set_xlim(left=0)

    fig = plt.gcf()
    fig.savefig(fig_file_name_png, dpi=300)
    fig.savefig(fig_file_name_pdf, dpi=300)

def plot_loss_ratio(df, m_type):
    title = 'RX packets miss ratio ({}){}'.format(m_type, TITLE_APPEND)
    fig_file_name_png = 'rx_miss_ratio_{}.png'.format(m_type)
    fig_file_name_pdf = 'rx_miss_ratio_{}.pdf'.format(m_type)

    # create path from file name
    fig_file_name_png = os.path.join(plot_dir, fig_file_name_png)
    fig_file_name_pdf = os.path.join(plot_dir, fig_file_name_pdf)
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
    
    df.plot(kind = 'line', ax=ax, x = 'time', y = 'ratio', marker='x', color='r', label='RX Miss Ratio')
    
    plt.xlabel('Time', fontproperties=FONT)
    plt.ylabel('Miss Ratio', fontproperties=FONT)
    ax.set_title(title, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))
    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
    ax.set_ylim(bottom=0, top=0.2)
    ax.set_xlim(left=0)

    fig = plt.gcf()
    fig.savefig(fig_file_name_png, dpi=300)
    fig.savefig(fig_file_name_pdf, dpi=300)

def plot_pps(df, m_type):
    title = 'Receive rate ({}){}'.format(m_type, TITLE_APPEND)
    fig_file_name_png = 'receive_rate_{}.png'.format(m_type)
    fig_file_name_pdf = 'receive_rate_{}.pdf'.format(m_type)

    # create path from file name
    fig_file_name_png = os.path.join(plot_dir, fig_file_name_png)
    fig_file_name_pdf = os.path.join(plot_dir, fig_file_name_pdf)

    # Convert to Mpps
    df['recv_pps'] = df['recv_pps'] / 1000000
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))
    
    df.plot(kind = 'line', ax=ax, x = 'time', y = 'recv_pps', marker='x', color='r', label='Receive rate')
    
    plt.xlabel('Time', fontproperties=FONT)
    plt.ylabel('Receive rate (Mpps)', fontproperties=FONT)
    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
    ax.set_title(title, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))
    ax.set_ylim(bottom=0, top=8)
    ax.set_xlim(left=0)

    fig = plt.gcf()
    fig.savefig(fig_file_name_png, dpi=300)
    fig.savefig(fig_file_name_pdf, dpi=300)

def graph():
    host_df = pd.read_csv(HOST_DATA)
    dpu_df = None
    
    if not host_only:
        dpu_df = pd.read_csv(DPU_DATA)

    plot_rx_miss(host_df, "host")
    plot_tx_miss(host_df, "host")
    plot_pps(host_df, "host")

    if not host_only:
        plot_rx_miss(dpu_df, "dpu")
        plot_tx_miss(dpu_df, "dpu")
        plot_pps(dpu_df, "dpu")
        plot_loss_ratio(dpu_df, "dpu")

def main():
    print("Make sure you have host_data.csv and dpu_data.csv stats files in the same directory")
    print("Proceed? (y/n)")
    if input() == 'y':
        graph()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Plot host and dpu port data')
    parser.add_argument('-d','--plot-dir', type=str, required=True, help='Directory containing data files')
    parser.add_argument('-a','--title-append', type=str, help='String append with every plot title')
    parser.add_argument('-x','--host-only', action='store_true', help='Only plot host data')
    
    args = parser.parse_args()
    plot_dir = args.plot_dir
    TITLE_APPEND = args.title_append
    host_only = args.host_only
    
    # Check if plot directory exists
    if not os.path.exists(plot_dir):
        print("Plot directory does not exist")
        exit(1)

    # Append directory to data file names
    HOST_DATA = os.path.join(plot_dir, HOST_DATA)
    DPU_DATA = os.path.join(plot_dir, DPU_DATA)

    main()
