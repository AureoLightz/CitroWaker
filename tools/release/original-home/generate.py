#!/usr/bin/env python3
"""Original CitroWaker release artwork. Pillow; no disc assets or web art."""
import argparse, math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

def background(size):
    w,h=size
    im=Image.new('RGB',size)
    for y in range(h):
        t=y/max(1,h-1)
        colour=tuple(round(a+(b-a)*t) for a,b in zip((12,31,47),(18,78,91)))
        ImageDraw.Draw(im).line((0,y,w,y),fill=colour)
    return im.convert('RGBA')

def emblem(size):
    s=size/256
    im=Image.new('RGBA',(size,size))
    d=ImageDraw.Draw(im)
    def line(points,colour,width):d.line([(round(x*s),round(y*s))for x,y in points],fill=colour,width=max(1,round(width*s)),joint='curve')
    # Open circular wind mark with three trailing strokes. Original geometry.
    points=[(119+72*math.cos(a),126+72*math.sin(a))for a in [math.radians(38+i*284/96)for i in range(97)]]
    line(points,(247,222,143,255),12)
    line([(112,105),(205,105)],(237,246,227,255),10)
    line([(101,129),(223,129)],(106,211,202,255),10)
    line([(112,153),(193,153)],(237,246,227,255),10)
    return im

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--font',type=Path,default=Path('/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf'))
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
    im=background((1024,512));im.alpha_composite(emblem(400),(12,56))
    d=ImageDraw.Draw(im);font=ImageFont.truetype(str(a.font),63)
    word='CitroWaker';box=d.textbbox((0,0),word,font=font);x=422;y=233-(box[1]+box[3])/2
    d.text((x+1,y+4),word,font=font,fill=(4,20,31));d.text((x,y),word,font=font,fill=(248,237,195))
    d.line((425,302,965,302),fill=(80,167,173),width=2)
    im.save(a.output/'banner-large.png');im.resize((256,128),Image.Resampling.LANCZOS).convert('RGB').save(a.output/'banner.png')
    for size in (24,48,256):
        icon=background((256,256));icon.alpha_composite(emblem(256));icon.resize((size,size),Image.Resampling.LANCZOS).convert('RGB').save(a.output/f'icon-{size}.png')
    emblem(512).save(a.output/'mark.png')

if __name__=='__main__':main()
