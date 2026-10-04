#!/usr/bin/env python3
"""Local RGBA-to-PNG preview. No receiver transport or firmware execution."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('rgba',type=Path); p.add_argument('png',type=Path)
p.add_argument('--video-context',action='store_true',help='synthetic background for alpha inspection')
a=p.parse_args()
pixels=a.rgba.read_bytes()
if len(pixels)!=720*480*4:
    p.error('expected tightly packed 720x480 RGBA bytes')
im=Image.frombytes('RGBA',(720,480),pixels)
if a.video_context:
    bg=Image.new('RGBA',im.size,(48,64,87,255)); d=ImageDraw.Draw(bg)
    for y in range(480):
        d.line((0,y,719,y),fill=(36+y//14,51+y//18,75+y//22,255))
    d.text((510,230),'SIMULATED VIDEO',fill=(150,170,190,255))
    im=Image.alpha_composite(bg,im)
a.png.parent.mkdir(parents=True,exist_ok=True); im.save(a.png)
