#!/usr/bin/env python3
"""彩色横纹(行噪声)分析工具。

把一组 JPEG 拆成 Y / Cb / Cr 三个通道, 逐行求均值得到行剖面(row profile),
再用高通(去掉场景本身的渐变)看"条纹"成分, 输出:

  * 亮度条 / 色度条 的幅度 (中位绝对行偏差) —— 判断是亮度噪声还是色度噪声;
  * 明区 / 暗区 分别的幅度 —— 判断是否集中在暗部(噪声底特征);
  * 条纹的空间周期 (行剖面自相关) —— 判断是否等于 JPEG MCU 行(8/16 行);
  * 最强条纹行的行号 mod 8 / mod 16 分布 —— 判断是否与 JPEG 块对齐;
  * 多帧之间行剖面的相关性 —— 固定图案(串扰/纹波) or 随机时间噪声。

只依赖 Pillow + numpy。

用法:
    python tools/pc_camera_stripe.py tools\\c24_*.jpg
    python tools/pc_camera_stripe.py tools\\avgframe_*.jpg --region dark
"""

from __future__ import annotations

import argparse
import glob
import sys

try:
    import numpy as np
    from PIL import Image
except ModuleNotFoundError as exc:  # pragma: no cover - 友好提示
    print(f"缺少依赖: {exc.name}", file=sys.stderr)
    print("请用 ESP-IDF 的 Python 跑, 或者:", file=sys.stderr)
    print(r"  C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe -m pip install pillow numpy",
          file=sys.stderr)
    raise SystemExit(2)


def load_ycc(path: str) -> np.ndarray:
    """返回 float32 的 HxWx3, 通道顺序 Y, Cb, Cr。"""
    with Image.open(path) as im:
        ycc = im.convert("YCbCr")
        return np.asarray(ycc, dtype=np.float32)


def high_pass_rows(plane: np.ndarray) -> np.ndarray:
    """沿垂直方向做 [1, -2, 1] 高通, 去掉场景本身的平滑渐变。

    返回形状 (H,), 是每行的"条纹强度"。
    """
    profile = plane.mean(axis=1)
    hp = np.zeros_like(profile)
    hp[1:-1] = profile[:-2] - 2.0 * profile[1:-1] + profile[2:]
    return hp


def row_energy(plane: np.ndarray) -> float:
    """逐行均值相对上一行的偏差中位数 —— 单一数字的"横纹指标"。"""
    profile = plane.mean(axis=1)
    return float(np.median(np.abs(np.diff(profile))))


def dark_rows(ycc: np.ndarray, frac: float = 0.35) -> np.ndarray:
    """按亮度取最暗的 frac 比例的行(整行平均)。"""
    y = ycc[:, :, 0].mean(axis=1)
    order = np.argsort(y)
    n = max(16, int(len(y) * frac))
    return np.sort(order[:n])


def dominant_period(signal: np.ndarray, max_lag: int = 64) -> tuple[int, float]:
    """行剖面的自相关主周期(去掉 lag=0 的尖峰)。"""
    x = signal - signal.mean()
    if not np.any(x):
        return 0, 0.0
    ac = np.correlate(x, x, mode="full")[len(x) - 1:]
    ac = ac / (ac[0] + 1e-9)
    lag = int(np.argmax(ac[2:max_lag]) + 2) if len(ac) > 2 else 0
    return lag, float(ac[lag])


def analyse(paths: list[str], region: str) -> None:
    profiles: list[tuple[str, dict[str, np.ndarray]]] = []
    summary: list[tuple[str, float, dict[str, float], dict[str, np.ndarray]]] = []

    for path in paths:
        ycc = load_ycc(path)
        h, w = ycc.shape[:2]
        rows = np.arange(h)
        if region == "dark":
            rows = dark_rows(ycc)
        elif region == "bright":
            y = ycc[:, :, 0].mean(axis=1)
            rows = np.sort(np.argsort(y)[-max(16, int(h * 0.35)):])

        prof = {}
        metric = {}
        for idx, name in enumerate(("Y", "Cb", "Cr")):
            plane = ycc[:, :, idx]
            prof[name] = high_pass_rows(plane[rows])
            metric[name] = row_energy(plane[rows])

        mean_y = float(ycc[rows, :, 0].mean())
        print(f"\n=== {path}  ({w}x{h}, region={region}, 平均亮度 {mean_y:.1f}) ===")
        print(f"  逐行起伏指标 (中位 |行差|):  Y={metric['Y']:.3f}"
              f"  Cb={metric['Cb']:.3f}  Cr={metric['Cr']:.3f}")
        print(f"  条纹幅度 (高通中位绝对值):    Y={np.median(np.abs(prof['Y'])):.3f}"
              f"  Cb={np.median(np.abs(prof['Cb'])):.3f}"
              f"  Cr={np.median(np.abs(prof['Cr'])):.3f}")

        # 色度相对亮度是否明显 —— 只要色度条明显, 就是"彩色"横纹
        chroma = np.hypot(prof["Cb"], prof["Cr"])
        luma = np.abs(prof["Y"])
        print(f"  彩色成分强度 / 亮度成分强度 = {np.median(chroma) / (np.median(luma) + 1e-6):.3f}")

        lag, acv = dominant_period(prof["Y"])
        print(f"  行剖面自相关主周期 ~ {lag} 行 (相关度 {acv:.2f})"
              f"   8行对齐残差={lag % 8}  16行对齐残差={lag % 16}")

        # 最强的那些条纹行落在 JPEG MCU 行的什么位置
        strong = np.argsort(-np.abs(prof["Y"]))[:20]
        mod8 = np.bincount(strong % 8, minlength=8)
        mod16 = np.bincount(strong % 16, minlength=16)
        print(f"  最强 20 行 mod 8 : {list(mod8)}")
        print(f"  最强 20 行 mod 16: {list(mod16)}")
        print(f"  mod8 最大占比 {mod8.max() / 20:.0%} / mod16 最大占比 {mod16.max() / 20:.0%}"
              "  (均匀=~12.5%/~6%, 集中=与 JPEG 块对齐)")

        profiles.append((path, prof))
        summary.append((path, mean_y, metric, prof))

    if len(profiles) < 2:
        return

    print("\n=== 汇总 ===")
    print(f"{'文件':<24}{'亮度':>7}{'行起伏Y':>9}{'条幅Y':>8}{'条幅Cb':>8}{'条幅Cr':>8}{'彩/亮':>7}")
    for path, mean_y, metric, prof in summary:
        luma = float(np.median(np.abs(prof["Y"])))
        chroma = float(np.median(np.hypot(prof["Cb"], prof["Cr"])))
        print(f"{path:<24}{mean_y:>7.1f}{metric['Y']:>9.3f}"
              f"{luma:>8.3f}{np.median(np.abs(prof['Cb'])):>8.3f}"
              f"{np.median(np.abs(prof['Cr'])):>8.3f}{chroma / (luma + 1e-6):>7.2f}")

    print("\n=== 帧间行剖面相关性 (固定图案 -> 接近 1, 随机噪声 -> 接近 0) ===")
    for name in ("Y", "Cb", "Cr"):
        vals = []
        for i in range(len(profiles) - 1):
            a, b = profiles[i][1][name], profiles[i + 1][1][name]
            n = min(len(a), len(b))
            x, y = a[:n] - a[:n].mean(), b[:n] - b[:n].mean()
            denom = np.sqrt((x ** 2).sum() * (y ** 2).sum())
            vals.append(float((x * y).sum() / denom) if denom else 0.0)
        arr = np.array(vals)
        print(f"  {name:>3}: 逐对 {[f'{v:+.2f}' for v in vals]}  ->  均值 {arr.mean():+.3f}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="JPEG/PNG 文件, 支持通配符")
    ap.add_argument("--region", choices=("all", "dark", "bright"), default="all",
                    help="只统计整幅 / 最暗 35%% / 最亮 35%% 的行")
    args = ap.parse_args()

    paths: list[str] = []
    for pat in args.files:
        hits = sorted(glob.glob(pat))
        paths.extend(hits if hits else [pat])

    analyse(paths, args.region)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
