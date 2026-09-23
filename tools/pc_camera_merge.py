#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把同一场景的连续多帧 JPEG 平均成一张, 用来压掉 OV2640 的逐行随机噪声(横纹)。

原理: 实测这种逐行起伏在帧与帧之间几乎不相关(相关系数 ~0), 属于随机时间噪声,
      把 N 帧对齐平均可以把噪声降到 1/√N (4 帧减半, 16 帧降到 1/4)。
      代价是要多拍几张、只适合静止画面。

用法:
    # 1) 先连拍几帧 (脚本会自动重采样对齐)
    python tools\\pc_camera_check.py 192.168.0.102 --frames 12 --out D:\\esp\\onegpio\\tools\\avg
    # 2) 合并
    python tools\\pc_camera_merge.py --files D:\\esp\\onegpio\\tools\\avg_*.jpg --out D:\\esp\\onegpio\\tools\\merged.png

    # 也可以一步到位 (内部调用 pc_camera_check.py 拍完再合并)
    python tools\\pc_camera_merge.py --host 192.168.0.102 --frames 12 --out D:\\esp\\onegpio\\tools\\merged.png
"""

import argparse
import glob
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))


def merge(files, out_path, jpeg_quality=95):
    imgs = []
    for f in files:
        try:
            im = Image.open(f)
            im.load()
            imgs.append(im.convert("RGB"))
        except Exception as exc:
            print("跳过 %s: %s" % (os.path.basename(f), exc))
    if len(imgs) < 2:
        print("至少需要 2 张可用的照片 (现在只有 %d 张)" % len(imgs))
        return 1

    w, h = imgs[0].size
    imgs = [im for im in imgs if im.size == (w, h)]
    n = len(imgs)

    # 逐步做加权平均: running = running*(1-1/k) + img_k*(1/k), 结果就是 N 帧均值,
    # 而且走的是 PIL 的 C 代码, 比逐字节 Python 循环快得多
    merged = imgs[0]
    for k, im in enumerate(imgs[1:], start=2):
        merged = Image.blend(merged, im, 1.0 / k)

    if out_path.lower().endswith((".jpg", ".jpeg")):
        merged.save(out_path, quality=jpeg_quality)
    else:
        merged.save(out_path)
    print("已合并 %d 张 (%dx%d) -> %s   噪声理论上降到 1/√%d ≈ %.2f 倍"
          % (n, w, h, out_path, n, 1.0 / (n ** 0.5)))
    return 0


def main():
    parser = argparse.ArgumentParser(description="多帧平均压横纹")
    parser.add_argument("--files", nargs="+", help="输入图片 (支持通配符)")
    parser.add_argument("--host", help="板子 IP: 直接用 pc_camera_check.py 拍一组再合并")
    parser.add_argument("--frames", type=int, default=12, help="配合 --host 时拍几帧 (默认 12)")
    parser.add_argument("--out", default="merged.png", help="输出文件 (默认 merged.png)")
    args = parser.parse_args()

    files = []
    if args.host:
        prefix = os.path.join(os.path.dirname(os.path.abspath(args.out)), "avgframe")
        cmd = [sys.executable, os.path.join(HERE, "pc_camera_check.py"), args.host,
               "--frames", str(args.frames), "--out", prefix]
        print("先拍 %d 帧: %s" % (args.frames, " ".join(cmd[1:])))
        rc = subprocess.call(cmd)
        if rc != 0:
            print("拍照失败 (返回 %d)" % rc)
            return rc
        files = sorted(glob.glob(prefix + "_*.jpg"))
    elif args.files:
        for pattern in args.files:
            files.extend(sorted(glob.glob(pattern)) if any(c in pattern for c in "*?[") else [pattern])
    else:
        parser.error("要么给 --files, 要么给 --host")

    return merge(files, args.out)


if __name__ == "__main__":
    sys.exit(main())
