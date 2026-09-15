#!/usr/bin/env python3

###############################
# Plot Thread Scaling Facets   #
###############################

import argparse
import os

import pandas as pd
from plotnine import (
    aes,
    element_blank,
    element_line,
    element_text,
    facet_grid,
    geom_line,
    geom_point,
    ggplot,
    guides,
    guide_legend,
    labs,
    scale_color_manual,
    scale_shape_manual,
    scale_linetype_manual,
    scale_x_continuous,
    scale_y_continuous,
    theme,
    theme_tufte,
)


MISS_PCT_THRESHOLD = 10.0
FIG_WIDTH = 2.5
FIG_HEIGHT = 1.5
LINE_SIZE = 0.2
POINT_SIZE = 0.7
POINT_STROKE = 0.25

SERIES_ORDER = ["NIC", "Host", "Combined", "Outback", "eRPC"]
SERIES_COLORS = {
    "NIC": "#e41a1c",       # ColorBrewer Set1 red
    "Host": "#377eb8",      # ColorBrewer Set1 blue
    "Combined": "#4daf4a",  # ColorBrewer Set1 green
    "Outback": "#984ea3",   # ColorBrewer Set1 purple
    "eRPC": "#ff7f00",      # ColorBrewer Set1 orange
}
SERIES_LINETYPES = {
    "NIC": "dotted",
    "Host": "dashed",
    "Combined": "solid",
    "Outback": "solid",
    "eRPC": "solid",
}
SERIES_SHAPES = {
    "NIC": "o",
    "Host": "^",
    "Combined": "s",
    "Outback": "D",
    "eRPC": "+",
}

WORKLOAD_LABELS = {
    "A": "YCSB-A",
    "B": "YCSB-B",
    "C": "YCSB-C",
}

SKEW_LABELS = {
    "uniform": "Uniform",
    "zipf": "Zipf Skew",
}


def peak_per_thread(group):
    """Return peak recv_mpps among valid low-miss operating points."""
    valid = group[group["miss_pct"] < MISS_PCT_THRESHOLD]
    if valid.empty:
        valid = group
    return valid.loc[valid["recv_mpps"].idxmax()]


def compute_peak(path):
    """Return peak per (workload, key_dist, threads) operating point for a CSV."""
    df = pd.read_csv(os.path.abspath(path))
    peak_rows = []
    for _, group in df.groupby(["workload", "key_dist", "threads"]):
        peak_rows.append(peak_per_thread(group))
    return (
        pd.DataFrame(peak_rows)
        .reset_index(drop=True)
        .sort_values(["key_dist", "workload", "threads"])
    )


def read_peak_dpu_host(path, dpu_path=None):
    host_peak = compute_peak(path)
    host_series = host_peak.assign(series="Host", throughput_mpps=host_peak["host_mpps"])

    frames = [host_series]

    if dpu_path:
        dpu_peak = compute_peak(dpu_path)
        nic_series = dpu_peak.assign(series="NIC", throughput_mpps=dpu_peak["dpu_mpps"])
        frames.append(nic_series)

        combined = pd.merge(
            dpu_peak[["workload", "key_dist", "threads", "dpu_mpps"]],
            host_peak[["workload", "key_dist", "threads", "host_mpps"]],
            on=["workload", "key_dist", "threads"],
            how="inner",
        )
        combined["series"] = "Combined"
        combined["throughput_mpps"] = combined["dpu_mpps"] + combined["host_mpps"]
        frames.append(combined)

    tidy = pd.concat(frames, ignore_index=True)
    return tidy[["workload", "key_dist", "threads", "series", "throughput_mpps"]]


def read_outback(path, max_threads=None):
    outback = pd.read_csv(os.path.abspath(path))
    outback = outback.rename(columns={"throughput_ops_per_sec": "throughput_mpps", "dist": "key_dist"})
    outback["throughput_mpps"] = outback["throughput_mpps"] / 1e6
    outback["workload"] = outback["workload"].str.replace("ycsb", "", case=False).str.upper()
    outback["key_dist"] = outback["key_dist"].replace({"zipfian": "zipf"})
    outback["series"] = "Outback"
    if "client_threads" in outback.columns:
        # Multiple client_threads counts were swept per server thread count;
        # keep only the best (peak) throughput for each operating point.
        idx = outback.groupby(["workload", "key_dist", "threads"])["throughput_mpps"].idxmax()
        outback = outback.loc[idx]
    if max_threads is not None:
        outback = outback[outback["threads"] <= max_threads]
    return outback[["workload", "key_dist", "threads", "series", "throughput_mpps"]]


def read_erpc(path, max_threads=None):
    erpc = pd.read_csv(os.path.abspath(path))
    rename = {"server_threads": "threads", "throughput_mrps_sum": "throughput_mpps", "throughput_mrps": "throughput_mpps"}
    rename = {k: v for k, v in rename.items() if k in erpc.columns}
    erpc = erpc.rename(columns=rename)
    if max_threads is not None:
        erpc = erpc[erpc["threads"] <= max_threads]
    erpc["workload"] = erpc["ycsb"].str.upper()
    erpc["key_dist"] = erpc["skew"]
    erpc["series"] = "eRPC"
    return erpc[["workload", "key_dist", "threads", "series", "throughput_mpps"]]


def add_labels(df):
    df = df.copy()
    df["workload_label"] = pd.Categorical(
        df["workload"].map(WORKLOAD_LABELS),
        categories=["YCSB-A", "YCSB-B", "YCSB-C"],
        ordered=True,
    )
    df["skew_label"] = pd.Categorical(
        df["key_dist"].map(SKEW_LABELS),
        categories=["Uniform", "Zipf Skew"],
        ordered=True,
    )
    df["series"] = pd.Categorical(df["series"], categories=SERIES_ORDER, ordered=True)
    return df


def thread_breaks(df):
    ticks = sorted(int(x) for x in df["threads"].dropna().unique())
    if len(ticks) <= 12:
        return ticks
    return [x for x in ticks if x == 0 or x == max(ticks) or x % 2 == 0]


def make_plot(df):
    return (
        ggplot(
            df,
            aes(
                "threads",
                "throughput_mpps",
                color="series",
                linetype="series",
                shape="series",
            ),
        )
        + geom_line(size=LINE_SIZE)
        + geom_point(size=POINT_SIZE, fill="white", stroke=POINT_STROKE)
        + facet_grid("skew_label ~ workload_label")
        + scale_color_manual(values=SERIES_COLORS, breaks=SERIES_ORDER, name="")
        + scale_linetype_manual(values=SERIES_LINETYPES, breaks=SERIES_ORDER, name="")
        + scale_shape_manual(values=SERIES_SHAPES, breaks=SERIES_ORDER, name="")
        + scale_x_continuous(
            breaks=thread_breaks(df),
            limits=(0, None),
            expand=(0, 0, 0.04, 0),
        )
        + scale_y_continuous(
            breaks=[0, 4, 8, 12, 16],
            limits=(0, 17),
            expand=(0, 0, 0, 0),
        )
        + labs(x="Host CPU Cores", y="Throughput (M op/s)", color="", linetype="")
        + theme_tufte(base_size=4.5)
        + theme(
            figure_size=(3.75, 1.375),
            panel_grid_major=element_blank(),
            panel_grid_minor=element_blank(),
            axis_line_x=element_line(color="black", size=0.4),
            axis_line_y=element_line(color="black", size=0.4),
            axis_ticks_major=element_line(color="black", size=0.3),
            axis_title=element_text(size=5),
            axis_text=element_text(size=4.5),
            axis_text_x=element_text(rotation=0, size=4.5),
            strip_background=element_blank(),
            strip_text=element_text(size=4.5),
            legend_position="top",
            legend_key=element_blank(),
            legend_text=element_text(size=5),
            legend_key_height=3,
            legend_key_width=8,
            legend_box_margin=0,
            legend_box_spacing=0,
            legend_margin=0,
            legend_spacing=0,
            plot_margin=0.01,
            subplots_adjust={"wspace": 0.16, "hspace": 0.18, "top": 0.62},
        )
        + guides(
            color=guide_legend(nrow=1, title=""),
            linetype=guide_legend(nrow=1, title=""),
            shape=guide_legend(nrow=1, title=""),
        )
    )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Plot all thread-scaling graphs as a plotnine facet grid"
    )
    parser.add_argument(
        "--dpu-host",
        default="results/thread_scaling_all/results.csv",
        help="Path to DPU+host results CSV",
    )
    parser.add_argument(
        "--dpu",
        default="results/thread_scaling_dpu_only2/results.csv",
        help="Path to DPU-only CSV; pass empty string to omit",
    )
    parser.add_argument(
        "--outback",
        default="results/outback/throughput.csv",
        help="Path to Outback throughput CSV; pass empty string to omit",
    )
    parser.add_argument(
        "--erpc",
        default="results/erpc/erpc-results.csv",
        help="Path to eRPC results CSV; pass empty string to omit",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="results/thread_scaling_all/thread_scaling_facets.pdf",
        help="Output PDF path",
    )
    parser.add_argument(
        "--max-threads",
        type=int,
        default=None,
        help="Maximum x-axis thread count for comparison lines; defaults to DPU+Host max",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    dpu_path = args.dpu or None
    df = read_peak_dpu_host(args.dpu_host, dpu_path=dpu_path)

    facets = sorted(df[["workload", "key_dist"]].drop_duplicates().itertuples(index=False, name=None))
    max_threads = args.max_threads
    if max_threads is None:
        max_threads = int(df["threads"].max())

    frames = [df]
    if args.outback:
        frames.append(read_outback(args.outback, max_threads=max_threads))
    if args.erpc:
        frames.append(read_erpc(args.erpc, max_threads=max_threads))

    plot_df = add_labels(pd.concat(frames, ignore_index=True))
    plot_df = plot_df.dropna(subset=["workload_label", "skew_label"])

    output = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    make_plot(plot_df).save(output, width=FIG_WIDTH, height=FIG_HEIGHT, units="in", verbose=False)
    print(f"Saved: {output}")


if __name__ == "__main__":
    main()
