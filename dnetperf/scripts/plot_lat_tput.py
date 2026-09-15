#!/usr/bin/env python3

# Script for plotting latency and tput graphs
# Usage: python plot_lat_tput.py <data_file>

import sys
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager
import matplotlib.ticker as ticker

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO

# Check number of arguments
if len(sys.argv) < 2:
    print("Usage: python plot_lat_tput.py <data_file> [y_bottom] [y_top]")
    sys.exit(1)

DATA_FILE = sys.argv[1]
Y_BOTTOM = int(sys.argv[2])
Y_TOP = int(sys.argv[3])
EXP_DIR = os.path.dirname(DATA_FILE)

df_lat_tput = pd.read_csv(DATA_FILE)
    
font = font_manager.FontProperties(family='Times New Roman', size=10)

def plot_latency():
    plots={}
    plots['mean'] = {'name': 'mean', 'column': 'lat_avg', 'ylabel': 'Mean'}
    plots['p50'] = {'name': 'median', 'column': 'lat_50th', 'ylabel': 'Median'}
    plots['p99'] = {'name': '99th', 'column': 'lat_99th', 'ylabel': '99th'}

    # Plot the latency vs throughput(req)
    for p in plots.values():
        fig_file_name_pdf = '{}/{}_latency.pdf'.format(EXP_DIR, p['name'])
        fig_file_name_png = '{}/{}_latency.png'.format(EXP_DIR, p['name'])

        fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

        df_lat_tput.plot(kind = 'line', ax=ax, x = 'tput_recv', y = p['column'], marker='x', color='r')

        plt.xlabel('Throughput (M req/s)', fontproperties=font)
        plt.ylabel('{} Latency (\u03BCs)'.format(p['ylabel']), fontproperties=font)
        #ax.legend(["x86 JIT", "x86 Interpreted"], prop=font)
        
        ax.set_yscale('log')

        # Set the y axis tick locator to be a logarithmic locator
        ax.yaxis.set_major_locator(ticker.LogLocator(base=10.0, subs='all', numticks=15))

        # Set the y axis tick formatter to be a function that returns the value as a string
        def y_fmt(y, _):
            return '{:g}'.format(y)

        ax.yaxis.set_major_formatter(ticker.FuncFormatter(y_fmt))
        
        ax.set_ylim(bottom=Y_BOTTOM, top=Y_TOP)
        ax.set_xlim(left=0)

        fig = plt.gcf()
        fig.savefig(fig_file_name_pdf, dpi=300)
        fig.savefig(fig_file_name_png, dpi=300)

def plot_throughput():
    # Plot throughput(resp) vs throughput(req)
    fig_file_name_pdf = '{}/tput.pdf'.format(EXP_DIR)
    fig_file_name_png = '{}/tput.png'.format(EXP_DIR)

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    df_lat_tput.plot(kind = 'line', ax=ax, x = 'tput_sent', y = 'tput_recv', marker='x', color='r')

    plt.xlabel('Throughput (M req/s)', fontproperties=font)
    plt.ylabel('Throughput (M resp/s)', fontproperties=font)
    #ax.legend(["x86 JIT", "x86 Interpreted"], prop=font)

    ax.set_ylim(bottom=0)
    ax.set_xlim(left=0)

    fig = plt.gcf()
    fig.savefig(fig_file_name_pdf, dpi=300)
    fig.savefig(fig_file_name_png, dpi=300)

def main():
    plot_latency()
    plot_throughput()

if __name__ == '__main__':
    main()
