#!/usr/bin/env python3
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont
import subprocess
root=Path(__file__).resolve().parents[1]
names=['home-jellyfin','home-doom','jellyfin-browser','jellyfin-details','iptv-browser','frigate-cameras','youtube-results','keyboard','player-osd','settings','loading','error','quick-connect']
(root/'previews').mkdir(exist_ok=True)
for name in names:
 raw=root/'build'/f'{name}.rgba'
 subprocess.run([str(root/'build/hr54-ui-host'),'--preview',name,'--output',str(raw)],check=True)
 image=Image.frombytes('RGBA',(720,480),raw.read_bytes());image.save(root/'previews'/f'{name}.png');raw.unlink()
 # Also verify aspect-preserving fitting at common output sizes.
 if name=='home-jellyfin':
  for w,h in [(1280,720),(1920,1080),(720,480)]:
   fit=Image.new('RGBA',(w,h),(0,0,0,0));scaled=image.copy();scaled.thumbnail((w,h),Image.Resampling.LANCZOS);fit.alpha_composite(scaled,((w-scaled.width)//2,(h-scaled.height)//2));fit.save(root/'previews'/f'geometry-{w}x{h}.png')
contact=Image.new('RGB',(1440,((len(names)+1)//2)*510),(18,18,18));draw=ImageDraw.Draw(contact);font=ImageFont.truetype(str(root/'assets/Overpass.ttf'),18)
for i,name in enumerate(names):
 image=Image.open(root/'previews'/f'{name}.png');contact.paste(image,(i%2*720,i//2*510),image);draw.text((i%2*720+20,i//2*510+480),name,font=font,fill=(180,195,205))
contact.save(root/'previews/contact-sheet.png')
print(f'{len(names)} production-renderer previews generated')
