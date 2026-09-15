#!/usr/bin/env python3

# Script for plotting multicore scaling
# Throughput vs number of cores

import sys
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.font_manager as font_manager

# Column width in latex template
LATEX_TEMPLATE_COLUMNWIDTH = 18

# Plot aspect ration
PLOT_ASPECT_RATIO = 16/6

PLOT_WIDTH = LATEX_TEMPLATE_COLUMNWIDTH
PLOT_HEIGHT = PLOT_WIDTH/PLOT_ASPECT_RATIO

PLOT_TITLE = "Multicore scaling (DPU)"

multicore_scaling_data = pd.read_csv('data/dpu_multicore_scaling.csv')

font = font_manager.FontProperties(family='Times New Roman', size=10)

fig_file_name = 'dpu_multicore_scaling.pdf'

fig, ax = plt.subplots(figsize=(PLOT_WIDTH, PLOT_HEIGHT))

multicore_scaling_data.plot(kind = 'line', ax=ax, x = 'core', y = 'tput', marker='x', color='r')

plt.xlabel('Cores', fontproperties=font)
plt.ylabel('Throughput (Mpps)', fontproperties=font)
ax.set_title(PLOT_TITLE, fontproperties=font_manager.FontProperties(family='Times New Roman', size=14, weight='bold'))
ax.set_ylim(bottom=0, top=5)
ax.set_xlim(left=1)

fig = plt.gcf()
fig.savefig(fig_file_name, dpi=300)
