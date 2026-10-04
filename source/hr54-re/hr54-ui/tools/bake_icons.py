#!/usr/bin/env python3
"""Bake source SVGs at 4x then Lanczos-filter to an embedded RGBA atlas.

Jellyfin's path and gradients are the unmodified upstream asset. Other symbols
are original shell artwork. No SVG parser or filesystem access runs on-box.
"""
from pathlib import Path
from PIL import Image
import cairosvg,io,math
root=Path(__file__).resolve().parents[1]
size=128
names=['jellyfin','iptv','frigate','youtube','doom']
with (root/'ui/icon_data.h').open('w') as out:
 out.write('/* Generated from the bundled source SVGs; attribution: assets/SOURCES.md. */\n#define ICON_SIZE 128\nstatic const unsigned char icon_rgba[5][128*128*4]={\n')
 for name in names:
  png=cairosvg.svg2png(url=str(root/'assets/icons'/f'{name}.svg'),output_width=size*4,output_height=size*4)
  image=Image.open(io.BytesIO(png)).convert('RGBA').resize((size,size),Image.Resampling.LANCZOS)
  image.save(root/'assets/icons'/f'{name}.png')
  data=image.tobytes();out.write('{\n')
  for i in range(0,len(data),64):out.write(','.join(map(str,data[i:i+64]))+',\n')
  out.write('},\n')
 out.write('};\n')
with (root/'ui/plasma_data.h').open('w') as out:
 out.write('/* Generated unit sine table; background uses integer arithmetic. */\nstatic const short plasma_sine[1024]={\n')
 for i in range(0,1024,32):out.write(','.join(str(round(math.sin(2*math.pi*k/1024)*1024)) for k in range(i,i+32))+',\n')
 out.write('};\n')
print('Baked five supersampled SVG icons and integer plasma table')

png=cairosvg.svg2png(url=str(root/'assets/preview-art.svg'),output_width=1000,output_height=640)
image=Image.open(io.BytesIO(png)).convert('RGBA').resize((250,160),Image.Resampling.LANCZOS)
with (root/'apps/preview_art.h').open('w') as out:
 out.write('/* Original SVG artwork, host previews only. */\nstatic const unsigned char preview_art[250*160*4]={\n')
 data=image.tobytes()
 for i in range(0,len(data),64):out.write(','.join(map(str,data[i:i+64]))+',\n')
 out.write('};\n')
