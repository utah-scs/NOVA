#!/usr/bin/env python3

# Script for plotting latency and tput graphs
# Usage: python plot_lat_tput.py <data_file>

import sys
import os
import pathlib
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager
import matplotlib.ticker as ticker
import matplotlib as mpl
from math import log2, log10, ceil

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

# PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
# PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO
PLOT_WIDTH = 3.38
# PLOT_WIDTH = 6
PLOT_HEIGHT = 3

# Script name
SCRIPT_NAME = os.path.basename(__file__)

# Check number of arguments
if len(sys.argv) != 3:
    print("Usage: {} <data_file>".format(SCRIPT_NAME))
    sys.exit(1)

DATA_FILE_NAAM = sys.argv[1]
DATA_FILE_IPIPE = sys.argv[2]

EXP_DIR = os.path.dirname(DATA_FILE_NAAM)
EXP_DIR = pathlib.Path(EXP_DIR).parent.absolute()

df_naam = pd.read_csv(DATA_FILE_NAAM)
df_ipipe = pd.read_csv(DATA_FILE_IPIPE)
    
SMALL_SIZE = 9
MEDIUM_SIZE = 10
BIGGER_SIZE = 12
# font = font_manager.FontProperties(family='Times New Roman', size=16)
# font = {'family' : 'Times New Roman', 'size'   : 10, 'weight' : 'normal'}
# mpl.rc('font', **font)
plt.rc('font', size=SMALL_SIZE)          # controls default text sizes
plt.rc('axes', titlesize=SMALL_SIZE)     # fontsize of the axes title
plt.rc('axes', labelsize=MEDIUM_SIZE)    # fontsize of the x and y labels
plt.rc('xtick', labelsize=SMALL_SIZE)    # fontsize of the tick labels
plt.rc('ytick', labelsize=SMALL_SIZE)    # fontsize of the tick labels
plt.rc('legend', fontsize=SMALL_SIZE)    # legend fontsize
plt.rc('figure', titlesize=BIGGER_SIZE)

X_COLUMN='functions'
X_LBL='Number of functions'
Y_COLUMN='lat_99th'
Y_LBL='99th latency (us)'

# Colors matching Figure 4 (ColorBrewer Set1 red/blue)
COLOR_IPIPE = '#E41A1C'
COLOR_NAAM = '#377EB8'

def plot_latency():
    fig_file_name_pdf = '{}/func_scaling.pdf'.format(EXP_DIR)
    fig_file_name_png = '{}/func_scaling.png'.format(EXP_DIR)

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    df_ipipe.plot(kind='line', ax=ax, x=X_COLUMN, y=Y_COLUMN,
                  linestyle='dashed', linewidth=1,
                  marker='x', markersize=8,
                  color=COLOR_IPIPE, label='iPipe')
    df_naam.plot(kind='line', ax=ax, x=X_COLUMN, y=Y_COLUMN,
                 linestyle='solid', linewidth=1,
                 marker='o', markersize=8,
                 markerfacecolor='none',
                 color=COLOR_NAAM, label='NOVA')

    plt.xlabel(X_LBL)
    plt.ylabel(Y_LBL)

    x_max = df_naam[X_COLUMN].max()
    ax.set_xscale('log', base=2)
    ax.xaxis.set_ticks([2**i for i in range(0, int(log2(x_max))+1)])
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())

    ax.set_yscale('log')
    ax.yaxis.set_major_formatter(ticker.ScalarFormatter())

    # Fixed range to match Figure 4 (values beyond 1000 are clipped,
    # same as iPipe's off-chart spike in the paper)
    ax.set_ylim(bottom=1, top=1000)
    ax.set_xlim(left=1)

    ax.tick_params(axis='both', which='both', direction='in')

    for spine in ax.spines.values():
        spine.set_linewidth(1.3)

    ax.legend(ncol=2, loc='lower left', bbox_to_anchor=(0, 1.02, 1, 0.2),
              mode='expand', borderaxespad=0, frameon=False)

    fig = plt.gcf()
    fig.tight_layout()
    fig.savefig(fig_file_name_pdf, dpi=300)
    fig.savefig(fig_file_name_png, dpi=300)

def main():
    plot_latency()

if __name__ == '__main__':
    main()
