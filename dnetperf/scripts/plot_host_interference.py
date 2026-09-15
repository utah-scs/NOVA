#!/usr/bin/env python3

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
from matplotlib import rcParams
import configparser
import argparse

PLOT_WIDTH_TPUT = 3.38
PLOT_HEIGHT_TPUT = 2

SECONDS_TO_MICROSECONDS = 1000000

DPU_COLOR        = '#e41a1c'  # red
HOST_COLOR       = '#377eb8'  # blue
DPU_LIMIT_COLOR  = '#ff7f00'  # orange
RULE_INSTALL_COLOR = '#984ea3' # purple

LINE_WIDTH = 0.5

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

# Font settings (kept for rtt/loss plots)
font = font_manager.FontProperties(family='Times New Roman', size=10)
    
parser = argparse.ArgumentParser(description='Plot Throughput, RTT, and Packet Loss graphs')
parser.add_argument('-f','--csv-file', type=str, help='Packets data file for the plots', required=True)
parser.add_argument('-c','--config-file', type=str, help='Graph configuration file', required=True)

args = parser.parse_args()
data_file = args.csv_file
config_file = args.config_file

config = configparser.ConfigParser()
config.read(config_file)

# Configuration variables
CSV_FILE_PATH = os.path.abspath(data_file)
DIR_NAME = os.path.dirname(CSV_FILE_PATH)
CSV_FILE = os.path.basename(CSV_FILE_PATH)
CSV_FILE_NAME = os.path.splitext(CSV_FILE)[0]
FILE_NAME_APPENDIX = config['DEFAULT']['FileNameAppendix']
EXP_RUN_TIME = int(config['DEFAULT']['ExperimentTime']) * SECONDS_TO_MICROSECONDS
X_AXIS_START = int(config['DEFAULT']['XAxisStart']) * SECONDS_TO_MICROSECONDS
X_AXIS_END = int(config['DEFAULT']['XAxisEnd']) * SECONDS_TO_MICROSECONDS
X_AXIS_STEP = float(config['DEFAULT']['XAxisStep']) * SECONDS_TO_MICROSECONDS

# Column names in the CSV file
COLUMN_SQN = 'pkt_sqn'
COLUMN_SEND_TIME = 'send_time (us)'
COLUMN_RTT = 'rtt (us)'
COLUMN_SERVER_TYPE = 'server_type'

# RTT Plot resolution in microseconds
PLOT_RESOLUTION_TPUT = int(config['TPUT']['PlotResolution'])
PLOT_RESOLUTION_RTT = int(config['RTT']['PlotResolution'])
PLOT_RESOLUTION_LOSS = int(config['LOSS']['PlotResolution'])

# X axis data
X_DATA_MIN = 0
X_DATA_MAX = EXP_RUN_TIME

# vertical line for rule installs
rules_install_times = config['DEFAULT']['RuleInstallTimes']
try:
    RULE_INSTALL_TIMES = [float(x) for x in rules_install_times.split(',')]
except:
    RULE_INSTALL_TIMES = []

# Maximum throughput of the DPU
try:
    MAX_DPU_TPUT = float(config['TPUT']['MaxDPUTput'])
except ValueError:
    MAX_DPU_TPUT = 0

# Plot the tput
def plot_tput(df):
    FIG_FILE_NAME_PDF = "{}/{}_tput{}.pdf".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
    FIG_FILE_NAME_PNG = "{}/{}_tput{}.png".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
    PLOT_TITLE = "Throughput " + config['DEFAULT']['CommonTitle']
    
    # Remove rows with zero values in rtt column
    df_no_loss = df[df[COLUMN_RTT] != 0]
    df_no_loss_host = df_no_loss[df_no_loss[COLUMN_SERVER_TYPE] != 0] 
    
    # Group original df by N microsecods
    bins = np.arange(df[COLUMN_SEND_TIME].min(), df[COLUMN_SEND_TIME].max() + PLOT_RESOLUTION_TPUT, PLOT_RESOLUTION_TPUT)
    
    df['send_time_groupby'] = pd.cut(df[COLUMN_SEND_TIME], bins)
    df_no_loss['send_time_groupby'] = pd.cut(df_no_loss[COLUMN_SEND_TIME], bins)
    df_no_loss_host['send_time_groupby'] = pd.cut(df_no_loss_host[COLUMN_SEND_TIME], bins)
    
    # Calculate offered Mpps
    # Total packets in each bins
    df_grouped = df.groupby('send_time_groupby').size().to_frame(name='counts')
    df_grouped.reset_index(drop=True, inplace=True)
    
    # Calculate offered Mpps for each bins
    df_grouped['counts'] = df_grouped['counts'].apply(lambda x: x / (PLOT_RESOLUTION_TPUT / SECONDS_TO_MICROSECONDS) / 1000000)
    
    # Calculate total received Mpps for each bins
    df_tput_grouped = df_no_loss.groupby('send_time_groupby').size().to_frame(name='counts')
    df_tput_grouped['counts'] = df_tput_grouped['counts'].apply(lambda x: x / (PLOT_RESOLUTION_TPUT / SECONDS_TO_MICROSECONDS) / 1000000)
    df_tput_grouped.reset_index(drop=True, inplace=True)

    # Calculate received mpps from host
    df_tput_host_grouped = df_no_loss_host.groupby('send_time_groupby').size().to_frame(name='counts')
    df_tput_host_grouped['counts'] = df_tput_host_grouped['counts'].apply(lambda x: x / (PLOT_RESOLUTION_TPUT / SECONDS_TO_MICROSECONDS) / 1000000)
    df_tput_host_grouped.reset_index(drop=True, inplace=True)
    
    df_new = pd.DataFrame()
    df_new['tput_sent'] = df_grouped['counts']
    df_new['tput_recv'] = df_tput_grouped['counts']
    df_new['tput_recv_host'] = df_tput_host_grouped['counts']
    
    df_tmp = pd.DataFrame(np.array([[0, 0, 0]]),
                 columns=['tput_sent', 'tput_recv', 'tput_recv_host'])
    df_new = pd.concat([df_tmp, df_new], ignore_index=True)
    
    times = range(0, len(df_new) * PLOT_RESOLUTION_TPUT, PLOT_RESOLUTION_TPUT)
    df_new['time'] = times

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH_TPUT, PLOT_HEIGHT_TPUT))

    # vertical line for rule installs
    for i in range(len(RULE_INSTALL_TIMES)):
        anot_str = "{}%".format(10*(i+1))
        if i == 0:
            plt.axvline(x=RULE_INSTALL_TIMES[i] * SECONDS_TO_MICROSECONDS, color=RULE_INSTALL_COLOR, linestyle='--', label='Rule Install', linewidth=LINE_WIDTH)
        else:
            plt.axvline(x=RULE_INSTALL_TIMES[i] * SECONDS_TO_MICROSECONDS, color=RULE_INSTALL_COLOR, linestyle='--', linewidth=LINE_WIDTH)
        plt.text((RULE_INSTALL_TIMES[i] + 1) * SECONDS_TO_MICROSECONDS, 6.5, anot_str, rotation=90)

    # Horizontal line for DPU limit
    if MAX_DPU_TPUT > 0:
        plt.axhline(y=MAX_DPU_TPUT, color=DPU_LIMIT_COLOR, linestyle=(0, (5, 10)), label='NIC Limit', linewidth=LINE_WIDTH)

    df_new.plot(x='time', y='tput_recv',      ax=ax, kind='line', color=DPU_COLOR,  label='Host + NIC', linewidth=LINE_WIDTH)
    df_new.plot(x='time', y='tput_recv_host', ax=ax, kind='line', color=HOST_COLOR, label='Host',       linewidth=LINE_WIDTH)

    plt.xlabel('Time (s)')
    plt.ylabel('Throughput (Mop/s)')

    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX + X_AXIS_STEP * 2, X_AXIS_STEP))
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(int(x/10**6))))

    ax.yaxis.set_ticks(np.arange(0, 9, 1))
    ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x)))

    # Scatter markers at each x-tick position
    for tick in plt.xticks()[0]:
        nearest = df_new.iloc[(df_new['time'] - tick).abs().argsort()[:1]]
        plt.scatter(nearest['time'], nearest['tput_recv'],      color=DPU_COLOR,  marker='o', s=15, facecolor='none')
        plt.scatter(nearest['time'], nearest['tput_recv_host'], color=HOST_COLOR, marker='x', s=15)

    ax.set_ylim(bottom=0, top=8)
    ax.set_xlim(left=X_AXIS_START, right=X_AXIS_END)

    line_comb  = Line2D([0], [0], color=DPU_COLOR,          marker='o', markerfacecolor='none', label='Host + NIC', linewidth=LINE_WIDTH)
    line_host  = Line2D([0], [0], color=HOST_COLOR,         marker='x',                         label='Host',       linewidth=LINE_WIDTH)
    line_limit = Line2D([0], [0], color=DPU_LIMIT_COLOR,                                        label='NIC Limit',  linewidth=LINE_WIDTH, linestyle=(0, (5, 10)))
    line_rule  = Line2D([0], [0], color=RULE_INSTALL_COLOR,                                     label='Split Traffic', linewidth=LINE_WIDTH, linestyle='--')
    plt.legend(handles=[line_comb, line_host, line_limit, line_rule],
               bbox_to_anchor=(0., 1.02, 1., .102), loc="lower left",
               ncols=2, mode="expand", borderaxespad=0., frameon=False)

    fig.savefig(FIG_FILE_NAME_PDF, dpi=300, bbox_inches='tight')
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300, bbox_inches='tight')

def plot_rtt(df):
    FIG_FILE_NAME_PDF = "{}/{}_rtt{}.pdf".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
    FIG_FILE_NAME_PNG = "{}/{}_rtt{}.png".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
    PLOT_TITLE = "Latency " + config['DEFAULT']['CommonTitle']
    
    # Remove rows with zero values in rtt column
    df_no_loss = df[df[COLUMN_RTT] != 0]
    df_no_loss_host = df_no_loss[df_no_loss[COLUMN_SERVER_TYPE] != 0] 
    df_no_loss_dpu = df_no_loss[df_no_loss[COLUMN_SERVER_TYPE] == 0]
    
    bins = np.arange(df[COLUMN_SEND_TIME].min(), df[COLUMN_SEND_TIME].max() + PLOT_RESOLUTION_RTT, PLOT_RESOLUTION_RTT)
    
    df_no_loss_host['send_time_groupby'] = pd.cut(df_no_loss_host[COLUMN_SEND_TIME], bins)
    df_no_loss_dpu['send_time_groupby'] = pd.cut(df_no_loss_dpu[COLUMN_SEND_TIME], bins)
    
    # Calculate average RTT for each bins
    df_rtt_host_grouped = df_no_loss_host.groupby('send_time_groupby').mean()
    df_rtt_host_grouped.reset_index(drop=True, inplace=True)
    df_rtt_dpu_grouped = df_no_loss_dpu.groupby('send_time_groupby').mean()
    df_rtt_dpu_grouped.reset_index(drop=True, inplace=True)
    
    df_new = pd.DataFrame()
    df_new['rtt_host'] = df_rtt_host_grouped[COLUMN_RTT]
    df_new['rtt_dpu'] = df_rtt_dpu_grouped[COLUMN_RTT]
    
    df_tmp = pd.DataFrame(np.array([[0, 0]]),
                 columns=['rtt_host', 'rtt_dpu'])
    df_new = pd.concat([df_tmp, df_new], ignore_index=True)
    
    times = range(0, len(df_new) * PLOT_RESOLUTION_RTT, PLOT_RESOLUTION_RTT)
    df_new['time'] = times
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH_TPUT, PLOT_HEIGHT_TPUT))

    # vertical line for rule installs
    for i in range(len(RULE_INSTALL_TIMES)):
        anot_str = "{}%".format(10*(i+1))
        if i == 0:
            plt.axvline(x=RULE_INSTALL_TIMES[i] * SECONDS_TO_MICROSECONDS, color='m', linestyle='--', label='Rule Install')
        else:
            plt.axvline(x=RULE_INSTALL_TIMES[i] * SECONDS_TO_MICROSECONDS, color='m', linestyle='--')
        plt.text((RULE_INSTALL_TIMES[i] + 1) * SECONDS_TO_MICROSECONDS, 2, anot_str, fontproperties=font, rotation=90)

    plt.legend()

    df_new.plot(x='time', y='rtt_dpu', ax=ax, kind='line', color=DPU_COLOR, label='RTT DPU')
    df_new.plot(x='time', y='rtt_host', ax=ax, kind='line', color=HOST_COLOR, label='RTT Host')
        
    plt.xlabel('Time (s)', fontproperties=font)
    plt.ylabel('RTT (\u03BCs)', fontproperties=font)

    # ax.set_title(PLOT_TITLE, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))

    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x/10**6)))
    # ax.xaxis.set_major_locator(ticker.MaxNLocator(integer=True, nbins=6))
    # ax.set_xticks(np.arange(0, 30 * 1000 * 1000, 5 * 1000 * 1000))

    ax.set_yscale('log')

    # Set the y axis tick locator to be a logarithmic locator
    ax.set_yticks([1, 10, 100, 1000, 10000])
    # ax.yaxis.set_major_locator(ticker.LogLocator(base=10.0, subs='all', numticks=8))

    # Set the y axis tick formatter to be a function that returns the value as a string
    def y_fmt(y, _):
        return '{:g}'.format(y)

    ax.yaxis.set_major_formatter(ticker.FuncFormatter(y_fmt))
        
    ax.set_ylim(bottom=0, top=50000)
    ax.set_xlim(left=X_AXIS_START, right=X_AXIS_END)
        
    fig.savefig(FIG_FILE_NAME_PDF, dpi=300)
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300)

def plot_loss_fraction(df): 
    FIG_FILE_NAME_PDF = "{}/{}_loss_frac{}.pdf".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
    FIG_FILE_NAME_PNG = "{}/{}_loss_frac{}.png".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
    PLOT_TITLE = "Loss Fraction " + config['DEFAULT']['CommonTitle']
    
    # Group original df by N microsecods
    bins = np.arange(df[COLUMN_SEND_TIME].min(), df[COLUMN_SEND_TIME].max() + PLOT_RESOLUTION_LOSS, PLOT_RESOLUTION_LOSS)
    
    df['send_time_groupby'] = pd.cut(df[COLUMN_SEND_TIME], bins)
    
    # Total packets in each bins
    df_grouped = df.groupby('send_time_groupby').size().to_frame(name='counts')
    df_grouped.reset_index(drop=True, inplace=True)

    # Packets loss in each bins
    df_grouped_loss = df.groupby('send_time_groupby').apply(lambda x: (x[COLUMN_RTT] == 0).sum()).to_frame(name='loss_count')
    df_grouped_loss.reset_index(drop=True, inplace=True)
    
    # Calculate fraction loss for each bins
    df_grouped_loss['loss_fraction'] = df_grouped_loss['loss_count'] / df_grouped['counts']
    
    df_new = pd.DataFrame()
    df_new['loss_fraction'] = df_grouped_loss['loss_fraction']
    
    df_tmp = pd.DataFrame(np.array([[0]]),
                 columns=['loss_fraction'])
    df_new = pd.concat([df_tmp, df_new], ignore_index=True)
    
    times = range(0, len(df_new) * PLOT_RESOLUTION_LOSS, PLOT_RESOLUTION_LOSS)
    df_new['time'] = times
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    # vertical line for rule installs
    for i in range(len(RULE_INSTALL_TIMES)):
        anot_str = "{}%".format(10*(i+1))
        if i == 0:
            plt.axvline(x=RULE_INSTALL_TIMES[i] * SECONDS_TO_MICROSECONDS, color='m', linestyle='--', label='Rule Install')
        else:
            plt.axvline(x=RULE_INSTALL_TIMES[i] * SECONDS_TO_MICROSECONDS, color='m', linestyle='--')
        plt.text((RULE_INSTALL_TIMES[i] + 1) * SECONDS_TO_MICROSECONDS, 20000, anot_str, fontproperties=font, rotation=90)

    plt.legend()

    df_new.plot(x='time', y='loss_fraction', ax=ax, kind='line', marker='x', color='red', label='Loss Fraction')
        
    plt.xlabel('Time (s)', fontproperties=font)
    plt.ylabel('Loss Fraction', fontproperties=font)

    ax.set_title(PLOT_TITLE, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))

    ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x/10**6)))
        
    ax.set_ylim(bottom=0, top=1)
    ax.set_xlim(left=X_AXIS_START, right=X_AXIS_END)
        
    fig = plt.gcf()
    fig.savefig(FIG_FILE_NAME_PDF, dpi=300)
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300)

def main():
    # Read the CSV file
    df = pd.read_csv(CSV_FILE_PATH)

    # Plot the graphs
    # print("Plotting Throughput")
    # plot_tput(df)
    print("Plotting Latency")
    plot_rtt(df)
    # print("Plotting Loss")
    # plot_loss_fraction(df)

if __name__ == "__main__":
    main()
