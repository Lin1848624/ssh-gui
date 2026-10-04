# SPDX-License-Identifier: GPL-3.0-or-later
# -*- coding: utf-8 -*-
"""生成 SshGui 的多尺寸图标（母版 4 倍超采样后 LANCZOS 缩小）。"""
import os
from PIL import Image, ImageDraw

OUT_DIR = r"D:\project\软件\ssh_gui\res"
DOC_DIR = r"D:\project\软件\ssh_gui\docs"
SS = 4
BASE = 256 * SS

img = Image.new("RGBA", (BASE, BASE), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# --- 对角渐变背景（蓝 -> 深靛） ---
c1 = (56, 139, 253)    # #388BFD 亮蓝
c2 = (13, 42, 102)     # #0D2A66 深靛
for y in range(BASE):
    for_t = y / (BASE - 1)
    col = tuple(int(c1[i] + (c2[i] - c1[i]) * for_t) for i in range(3))
    d.line([(0, y), (BASE, y)], fill=col + (255,))

# 左上角加一点高光，让渐变不那么平
glow = Image.new("RGBA", (BASE, BASE), (0, 0, 0, 0))
gd = ImageDraw.Draw(glow)
gd.ellipse([-BASE * 0.35, -BASE * 0.55, BASE * 0.85, BASE * 0.45], fill=(255, 255, 255, 46))
img = Image.alpha_composite(img, glow)

# --- 圆角遮罩 ---
mask = Image.new("L", (BASE, BASE), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, BASE - 1, BASE - 1],
                                       radius=int(BASE * 0.22), fill=255)
img.putalpha(mask)

d = ImageDraw.Draw(img)

# --- 终端提示符 ">_" ---
W = int(BASE * 0.072)          # 笔画宽度
white = (255, 255, 255, 255)
cyan = (125, 231, 214, 255)    # 光标块用青色，做视觉焦点

# ">" 折线
chev = [(int(BASE * 0.26), int(BASE * 0.30)),
        (int(BASE * 0.46), int(BASE * 0.50)),
        (int(BASE * 0.26), int(BASE * 0.70))]
d.line(chev, fill=white, width=W, joint="curve")
# 折线两端补圆头
for pt in (chev[0], chev[2]):
    d.ellipse([pt[0] - W // 2, pt[1] - W // 2, pt[0] + W // 2, pt[1] + W // 2], fill=white)

# "_" 光标块
d.rounded_rectangle([int(BASE * 0.55), int(BASE * 0.62),
                     int(BASE * 0.80), int(BASE * 0.62) + W],
                    radius=W // 3, fill=cyan)

master = img.resize((256, 256), Image.LANCZOS)
master.save(os.path.join(OUT_DIR, "app.ico"), format="ICO",
            sizes=[(s, s) for s in [16, 20, 24, 32, 40, 48, 64, 128, 256]])
master.save(os.path.join(DOC_DIR, "icon.png"), format="PNG")
print("OK ->", os.path.join(OUT_DIR, "app.ico"))
