#!/usr/bin/env python

import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO

x86_jit_path = './results/x86_jit.csv'
x86_nojit_path = './results/x86_nojit.csv'

df_x86_jit = pd.read_csv(x86_jit_path)
df_x86_nojit = pd.read_csv(x86_nojit_path)

plots={}
plots['mean'] = {'name': 'mean', 'column': 'avg lat', 'ylabel': 'Mean'}
plots['p50'] = {'name': 'median', 'column': 'p50 lat', 'ylabel': 'Median'}
plots['p99'] = {'name': '99th', 'column': 'p99 lat', 'ylabel': '99th'}

font = font_manager.FontProperties(family='Times New Roman', size=10)

for p in plots.values():
    fig_file_name = '{}_latency.pdf'.format(p['name'])

    fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

    df_x86_jit.plot(kind = 'line', ax=ax, x = 'tput', y = p['column'], marker='x', color='r')
    df_x86_nojit.plot(kind = 'line', ax=ax, x = 'tput', y = p['column'], marker='x', color='r', linestyle='dashed')

    plt.xlabel('Throughput (M req/s)', fontproperties=font)
    plt.ylabel('{} Latency (\u03BCs)'.format(p['ylabel']), fontproperties=font)
    ax.legend(["x86 JIT", "x86 Interpreted"], prop=font)
    ax.set_ylim(bottom=0, top=120)
    ax.set_xlim(left=0)

    fig = plt.gcf()
    fig.savefig(fig_file_name, dpi=300)
