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
import configparser
import argparse

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO

SECONDS_TO_NANOSECONDS = 1000000000

# Font settings
font = font_manager.FontProperties(family='Times New Roman', size=10)
    
# Configuration variables
CSV_FILE_PATH = "cycles.csv"
EXP_RUN_TIME = 30 * SECONDS_TO_NANOSECONDS
X_AXIS_START = 0 * SECONDS_TO_NANOSECONDS
X_AXIS_END = 30 * SECONDS_TO_NANOSECONDS
X_AXIS_STEP = 1 * SECONDS_TO_NANOSECONDS

# Column names in the CSV file
COLUMN_TIME = 'time'
COLUMN_CYCLES = 'cycles'

# RTT Plot resolution in nanoseconds
PLOT_RESOLUTION = 10000000

def plot_cycles(df):
    FIG_FILE_NAME_PNG = "cycles.png"
    PLOT_TITLE = "Interference, 5 Mpps load"

    bins = np.arange(df[COLUMN_TIME].min(), df[COLUMN_TIME].max() + PLOT_RESOLUTION, PLOT_RESOLUTION)

    # Calculate average cycles for each bins
    df_grouped = df.groupby(pd.cut(df[COLUMN_TIME], bins)).mean()
    df_grouped.reset_index(drop=True, inplace=True)
    
    df_new = pd.DataFrame()
    df_new['cycles'] = df_grouped[COLUMN_CYCLES]
    
    times = range(df[COLUMN_TIME].min(), df[COLUMN_TIME].max(), PLOT_RESOLUTION)
    df_new['time'] = times
    
    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    df_new.plot(x='time', y='cycles', ax=ax, kind='line', marker='x', color='red', label='Cycles')
        
    plt.xlabel('Time (s)', fontproperties=font)
    plt.ylabel('Cycles', fontproperties=font)

    ax.set_title(PLOT_TITLE, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))

    ax.xaxis.set_ticks(np.arange(0, df[COLUMN_TIME].max(), X_AXIS_STEP))
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, pos: '{:}'.format(x/10**9)))

    ax.set_yscale('log')

    # Set the y axis tick locator to be a logarithmic locator
    ax.set_yticks([100, 500, 1000, 10000, 1500, 20000, 30000, 50000])

    # Set the y axis tick formatter to be a function that returns the value as a string
    def y_fmt(y, _):
        return '{:g}'.format(y)

    ax.yaxis.set_major_formatter(ticker.FuncFormatter(y_fmt))
        
    ax.set_ylim(bottom=50, top=50000)
    # ax.set_xlim(left=18 * SECONDS_TO_NANOSECONDS, right=19 * SECONDS_TO_NANOSECONDS)
    ax.set_xlim(left=15 * SECONDS_TO_NANOSECONDS, right=20 * SECONDS_TO_NANOSECONDS)
    # ax.set_xlim(left=0, right=df[COLUMN_TIME].max())
        
    fig.savefig(FIG_FILE_NAME_PNG, dpi=300)

def main():
    # Read the CSV file
    df = pd.read_csv(CSV_FILE_PATH)

    print("min time: ", df[COLUMN_TIME].min())
    print("max time: ", df[COLUMN_TIME].max())

    # Plot the graphs
    print("Plotting cycles")
    plot_cycles(df)

if __name__ == "__main__":
    main()
