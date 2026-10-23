#!/usr/bin/env python3
"""把 PPM(P6) 图片转换成 PNG：python3 tools/ppm2png.py in.ppm [out.png]
（离线渲染测试输出 PPM，因为写 PPM 不需要任何库）"""
import sys
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"
Image.open(src).save(dst)
print(dst)
