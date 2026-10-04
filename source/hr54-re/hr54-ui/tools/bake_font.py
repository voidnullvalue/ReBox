#!/usr/bin/env python3
"""Reproducible anti-aliased masks; runtime has no font/stock-library dependency."""
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path
from fontTools.ttLib import TTFont
root=Path(__file__).resolve().parents[1]
points=list(range(32,592))+list(range(880,1280))+[0x2013,0x2014,0x2018,0x2019,0x201c,0x201d,0x2026,0x203a,0x25b6,0x25c0,0x2190,0x2192,0x2193]
data=bytearray(); glyphs=[]; supersample=3
supported=TTFont(root/'assets/Overpass.ttf').getBestCmap()
for size in (14,18,22,30,36):
 font=ImageFont.truetype(str(root/'assets/Overpass.ttf'),size*supersample)
 font.set_variation_by_axes([300 if size>=30 else 400])
 fallback=ImageFont.truetype(str(root/'assets/NotoSans-Regular.ttf'),size*supersample)
 for cp in points:
  face=font if cp in supported else fallback
  char=chr(cp); raw=face.getbbox(char,anchor='la'); box=(raw[0]//supersample,raw[1]//supersample,(raw[2]+supersample-1)//supersample,(raw[3]+supersample-1)//supersample)
  w=max(1,box[2]-box[0]);h=max(1,box[3]-box[1]);im=Image.new('L',(w*supersample,h*supersample));ImageDraw.Draw(im).text((-box[0]*supersample,-box[1]*supersample),char,font=face,fill=255,anchor='la');im=im.resize((w,h),Image.Resampling.LANCZOS)
  glyphs.append((cp,len(data),w,h,box[0],box[1],round(face.getlength(char)/supersample)))
  data.extend(im.tobytes())
with (root/'ui/font_data.h').open('w') as f:
 f.write(f'/* Generated from bundled Overpass; unsupported scripts use Noto Sans. See assets/SOURCES.md and OFL licenses. */\n#define FONT_GLYPHS {len(points)}\nstatic const unsigned char font_mask[]={{\n')
 for i in range(0,len(data),32):f.write(','.join(map(str,data[i:i+32]))+',\n')
 f.write('};\nstatic const Glyph font_glyphs[][FONT_GLYPHS]={\n')
 for i in range(0,len(glyphs),len(points)):
  f.write('{'+','.join('{'+','.join(map(str,g))+'}' for g in glyphs[i:i+len(points)])+'},\n')
 f.write('};\n')
print(f'baked {len(glyphs)} glyphs / {len(data)} mask bytes')
