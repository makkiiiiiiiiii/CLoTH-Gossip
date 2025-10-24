import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np

# ファイルパス（必要に応じて変更）
summary_path = "../result/summary.csv"
df = pd.read_csv(summary_path, encoding="utf-8", engine="python")

# center, alpha を抽出
df["center"] = df["simulation_id"].str.extract(r'center=([0-9.]+)').astype(float)
df["alpha"] = df["simulation_id"].str.extract(r'alpha=([0-9.]+)').astype(float)
df_grouped = df.groupby(["alpha", "center"]).mean(numeric_only=True).reset_index()

# ヒートマップ描画関数（フォーマット指定可能）
def plot_heatmap(data, value_col, filename, label, fmt=".4f", scientific=False):
    pivot = data.pivot(index="alpha", columns="center", values=value_col)
    plt.figure(figsize=(8, 6))

    # 指数表記に変換（必要な場合）
    if scientific:
        annot = pivot.applymap(lambda x: f"{x:.1e}")
        sns.heatmap(pivot, annot=annot, fmt="", cmap="coolwarm", cbar_kws={"label": label})
    else:
        sns.heatmap(pivot, annot=True, fmt=fmt, cmap="coolwarm", cbar_kws={"label": label})

    plt.title(f"{label} Heatmap")
    plt.xlabel("center")
    plt.ylabel("α")
    plt.xticks(rotation=45)
    plt.yticks(rotation=0)
    plt.tight_layout()
    plt.savefig(filename)
    plt.close()

# 保存先ディレクトリ
outdir = "../result/"

# ==== ヒートマップ作成 ====
plot_heatmap(df_grouped, "success_rate", outdir + "heatmap_success_rate.pdf", "Success Rate", fmt=".4f")
plot_heatmap(df_grouped, "group_capacity/95-percentile", outdir + "heatmap_group_capacity_95p.pdf", "Group Capacity (95th Percentile)", scientific=True)
plot_heatmap(df_grouped, "fail_no_alternative_path_rate", outdir + "heatmap_fasr.pdf", "FASR", fmt=".4f")
plot_heatmap(df_grouped, "time_success/average", outdir + "heatmap_latency_success.pdf", "Latency (Success) [ms]", fmt=".0f")
plot_heatmap(df_grouped, "time_fail/average", outdir + "heatmap_latency_fail.pdf", "Latency (Fail) [ms]", fmt=".0f")
plot_heatmap(df_grouped, "group_cover_rate", outdir + "heatmap_group_cover_rate.pdf", "Group Cover Rate", fmt=".4f")
plot_heatmap(df_grouped, "retry/average", outdir + "heatmap_retry_average.pdf", "Retry Average", fmt=".4f")
plot_heatmap(df_grouped, "cul/average", outdir + "heatmap_cul_average.pdf", "CUL Average", fmt=".4f")
plot_heatmap(df_grouped, "time_success/average", outdir + "heatmap_time_success_average.pdf", "Time Success Average [ms]", fmt=".2f")