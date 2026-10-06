#!/usr/bin/env python3
"""Bake shell-owned background and host preview art.

Module icons belong to module packages and are decoded at runtime.
"""
from pathlib import Path
from PIL import Image
import cairosvg,io,math
root=Path(__file__).resolve().parents[1]
size=128
with (root/'ui/plasma_data.h').open('w') as out:
 out.write('/* Generated unit sine table; background uses integer arithmetic. */\nstatic const short plasma_sine[1024]={\n')
 for i in range(0,1024,32):out.write(','.join(str(round(math.sin(2*math.pi*k/1024)*1024)) for k in range(i,i+32))+',\n')
 out.write('};\n')
print('Baked shell background table')

png=cairosvg.svg2png(url=str(root/'assets/preview-art.svg'),output_width=1000,output_height=640)
image=Image.open(io.BytesIO(png)).convert('RGBA').resize((250,160),Image.Resampling.LANCZOS)
with (root/'apps/preview_art.h').open('w') as out:
 out.write('/* Original SVG artwork, host previews only. */\nstatic const unsigned char preview_art[250*160*4]={\n')
 data=image.tobytes()
 for i in range(0,len(data),64):out.write(','.join(map(str,data[i:i+64]))+',\n')
 out.write('};\n')
