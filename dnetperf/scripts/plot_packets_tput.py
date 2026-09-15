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

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO

SECONDS_TO_MICROSECONDS = 1000000

# Check number of arguments
if len(sys.argv) != 8:
    print(len(sys.argv))
    print("Usage: plot_packets_tput.py <csv_file> <file_name_append> <plot_title> <exp_run_time> <x_axis_start> <x_axis_end> <x_axis_step>")
    sys.exit(1)

# Configure this variables
CSV_FILE_PATH = sys.argv[1]
DIR_NAME = os.path.dirname(CSV_FILE_PATH)
CSV_FILE = os.path.basename(CSV_FILE_PATH)
CSV_FILE_NAME = os.path.splitext(CSV_FILE)[0]
FILE_NAME_APPENDIX = sys.argv[2]
FIG_FILE_NAME = "{}/{}_tput{}.pdf".format(DIR_NAME, CSV_FILE_NAME, FILE_NAME_APPENDIX)
PLOT_TITLE = sys.argv[3]
EXP_RUN_TIME = int(sys.argv[4]) * SECONDS_TO_MICROSECONDS
X_AXIS_START = int(sys.argv[5]) * SECONDS_TO_MICROSECONDS
X_AXIS_END = int(sys.argv[6]) * SECONDS_TO_MICROSECONDS
X_AXIS_STEP = float(sys.argv[7]) * SECONDS_TO_MICROSECONDS

# column names
COLUMN_SQN = 'pkt_sqn'
COLUMN_SEND_TIME = 'send_time (us)'
COLUMN_RTT = 'rtt (us)'

# RTT Plot resolution in microseconds
PLOT_RESOLUTION = 10000

# X axis data
X_DATA_MIN = 0
X_DATA_MAX = int(sys.argv[4]) * SECONDS_TO_MICROSECONDS

# Read the data from the CSV file
df = pd.read_csv(CSV_FILE_PATH)

# Font settings
font = font_manager.FontProperties(family='Times New Roman', size=10)

# Remove rows with zero values in rtt column
df_rtt = df[df[COLUMN_RTT] != 0]

# Group by N microsecods
bins = np.arange(df_rtt[COLUMN_SEND_TIME].min(), df_rtt[COLUMN_SEND_TIME].max() + PLOT_RESOLUTION, PLOT_RESOLUTION)

df_rtt['send_time_groupby'] = pd.cut(df_rtt[COLUMN_SEND_TIME], bins)

# Calculate Mpps for each bins
df_rtt_grouped = df_rtt.groupby('send_time_groupby').size().to_frame(name='counts')
df_rtt_grouped['counts'] = df_rtt_grouped['counts'].apply(lambda x: x / (PLOT_RESOLUTION / SECONDS_TO_MICROSECONDS) / 1000000)

tput = df_rtt_grouped['counts'].tolist()
tput.insert(0, 0)

times = range(0, EXP_RUN_TIME + PLOT_RESOLUTION, PLOT_RESOLUTION)
df_final = pd.DataFrame({"time": times, "tput": tput})

fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

df_final.plot(x='time', y='tput', ax=ax, kind='line', marker='x', color='red', label='Throughput')
    
plt.xlabel('Time (s)', fontproperties=font)
plt.ylabel('Throughput (Mpps)', fontproperties=font)

ax.set_title(PLOT_TITLE, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))

ax.xaxis.set_ticks(np.arange(X_DATA_MIN, X_DATA_MAX, X_AXIS_STEP))
ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x/10**6)))

ax.set_ylim(bottom=0, top=8)
ax.set_xlim(left=X_AXIS_START, right=X_AXIS_END)
    
fig = plt.gcf()
fig.savefig(FIG_FILE_NAME, dpi=300)
