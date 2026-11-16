from PIL import Image,ImageDraw,ImageFont
from pathlib import Path
import argparse
p=argparse.ArgumentParser();p.add_argument('--font',required=True);p.add_argument('--output',required=True);a=p.parse_args();font=ImageFont.truetype(a.font,13)
mask=[];adv=[]
for c in range(32,127):
 im=Image.new('L',(14,18));ImageDraw.Draw(im).text((0,-2),chr(c),font=font,fill=255);mask+=list(im.getdata());adv.append(min(14,max(3,round(font.getlength(chr(c))))))
o=Path(a.output);o.write_text('#pragma once\n#include <cstdint>\n// Noto Sans rasterized at build time; see presentation/settings/README.md.\ninline constexpr uint8_t kSettingsAdvance[]={'+','.join(map(str,adv))+'};\ninline constexpr uint8_t kSettingsGlyph[]={\n'+',\n'.join(','.join(map(str,mask[i:i+84])) for i in range(0,len(mask),84))+'};\n')
