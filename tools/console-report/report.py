#!/usr/bin/env python3
"""Local, standard-library console-log report. Numbers retain their sample scope."""
import argparse
from collections import Counter
from pathlib import Path
import re
import statistics
import textwrap

NUM = r'([0-9]+(?:\.[0-9]+)?)'


def parse(path, start=0):
    lines=Path(path).read_text(errors='replace').splitlines()
    r={'path':str(path),'build':[],'perf':[],'present':[],'frames':[],'memory':[],
       'depth':[],'captures':[],'textures':[],'split':[],'game':[],'warnings':[]}
    for line in lines:
        if line.startswith('[build]'):r['build'].append(line[8:])
        if line.startswith('[perf]'):r['perf'].append(line[7:])
        m=re.match(r'\[present\] frame (\d+): (\d+) intervals mean '+NUM+r' ms \('+NUM+r' fps\) min '+NUM+r' max '+NUM,line)
        if m and int(m[1])>=start:
            frame,n=int(m[1]),int(m[2]);mean=float(m[3])
            if n>0 and mean>0:r['present'].append((frame,n,mean,float(m[5]),float(m[6])))
        m=re.match(r'\[3ds\] frame (\d+):',line)
        if m and int(m[1])>=start:
            vals={'frame':int(m[1])}
            for key in ['process','render','build','plan','draw','present']:
                v=re.search(r'\b'+key+r' '+NUM,line)
                if v:vals[key]=float(v[1])
            d=re.search(r': (\d+)/(\d+) draws',line)
            if d:vals['drawn']=int(d[1]);vals['draws']=int(d[2])
            r['frames'].append(vals)
        m=re.search(r'\[mem\] host heap: in use (\d+) KB, taken from the system (\d+) KB of (\d+) KB \(sbrk room (\d+) KB\)',line)
        if m:r['memory'].append(tuple(map(int,m.groups())))
        m=re.match(r'\[depth-probe\] (.*?)\s+[ZW] scale .*?: centre ([0-9a-fA-F]+).*? expect ([0-9a-fA-F]+)',line)
        if m:r['depth'].append((m[1],int(m[3],16),int(m[2],16)))
        if line.startswith('[capture]'):r['captures'].append(line)
        if line.startswith(('[texprof]','[texpack]')):r['textures'].append(line)
        if line.startswith(('[render-split]','[render-split2]')):
            m=re.search(r'frame (\d+)',line)
            if m and int(m[1])>=start:r['split'].append(line)
        if line.startswith(('[game-prof]','[game-wait]')):r['game'].append(line)
    frames=[p[0] for p in r['present']]
    if any(b<=a for a,b in zip(frames,frames[1:])):r['warnings'].append('Frame counter resets/repeats: combined blocks may span sessions.')
    if r['captures']:r['warnings'].append('Capture/texture-dump stalls may contaminate timing blocks; no automatic exclusion.')
    return r


def metrics(r):
    p=r['present'];n=sum(v[1] for v in p);ms=sum(v[1]*v[2] for v in p)/n if n else None
    def median(key):
        a=[x[key] for x in r['frames'] if key in x]
        return statistics.median(a) if a else None
    return {'block-weighted frame ms':ms,'block-weighted FPS':1000/ms if ms else None,
            'sampled process median ms':median('process'),'sampled render median ms':median('render'),
            'logged heap peak KB':max((m[0] for m in r['memory']),default=None),
            'logged min sbrk room KB':min((m[3] for m in r['memory']),default=None)}


def fmt(v):return 'not logged' if v is None else f'{v:.2f}'


def summary(r, full_depth=False):
    out=['# '+Path(r['path']).parent.name+' / '+Path(r['path']).name]
    for b in r['build']:out.extend(textwrap.wrap('Build: '+b,112))
    out+=['','Timings: console-clock log values; verify provenance. All selected blocks include menus/loads.']
    out+=['Process/render are inclusive wall times on different cores, NOT additive CPU/GPU costs.']
    for k,v in metrics(r).items():out.append(f'{k}: {fmt(v)}')
    out+=['','60-frame [present] blocks (actual interval counts shown):','frame | n | mean ms | FPS | min / max ms']
    for frame,n,mean,lo,hi in r['present']:out.append(f'{frame:5} | {n:3} | {mean:7.2f} | {1000/mean:6.2f} | {lo:.1f} / {hi:.1f}')
    if not r['present']:out.append('No presentation blocks logged.')
    out+=['','Last sparse frame sample (NOT a 60-frame average):',str(r['frames'][-1]) if r['frames'] else 'not logged']
    out+=['Game-thread split: '+('see logged profiler lines below' if r['game'] else 'not logged; cannot infer it by subtracting overlapping cores.')]
    for label,key,limit in [('Perf settings','perf',None),('Render detail (latest samples)','split',4),('Game profiler (latest lines)','game',15),('Captures','captures',None)]:
        if r[key]:out+=['',label+':']+r[key][-limit:] if limit else ['',label+':']+r[key]
    if r['textures']:
        out+=['','Texture pack / conversion blocks (latest of each):']
        for tag in ['[texpack]','[texprof]']:
            out += [line for line in r['textures'] if line.startswith(tag)][-2:]
    out+=['','Depth probe: expected / stored, hexadecimal (raw probe expectations, not scene depth):']
    d=r['depth'];counts=Counter('exact' if x==y else '±1' if abs(x-y)<=1 else 'mismatch' for _,x,y in d)
    out.append(f'{len(d)} probes: '+', '.join(f'{k} {counts[k]}' for k in ['exact','±1','mismatch']))
    selected=d if full_depth else sorted(d,key=lambda a:abs(a[1]-a[2]),reverse=True)[:6]
    for name,expected,stored in selected:out.append(f'{name:32} {expected:06x} / {stored:06x}')
    if len(d)>len(selected):out.append('Largest differences shown; --full-depth prints every probe.')
    for w in r['warnings']:out.append('NOTE: '+w)
    return '\n'.join(out)+'\n'


def compare(a,b):
    x,y=metrics(a),metrics(b)
    out=['# Log comparison','Different routes/settings/capture activity are NOT a controlled performance A/B.',
         f'A: {a["path"]}',f'B: {b["path"]}','metric | A | B | B − A']
    for key in x:
        delta=y[key]-x[key] if x[key] is not None and y[key] is not None else None
        out.append(f'{key} | {fmt(x[key])} | {fmt(y[key])} | {fmt(delta)}')
    out+=['','Presentation blocks by frame (not guaranteed to be the same scene):','frame | A ms / FPS | B ms / FPS']
    left={p[0]:p for p in a['present']};right={p[0]:p for p in b['present']}
    def show(p):return f'{p[2]:.2f} / {1000/p[2]:.2f}' if p else 'not logged'
    for frame in sorted(left.keys()|right.keys()):out.append(f'{frame} | {show(left.get(frame))} | {show(right.get(frame))}')
    return '\n'.join(out)+'\n'


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('log');ap.add_argument('log2',nargs='?');ap.add_argument('--start-frame',type=int,default=0);ap.add_argument('--full-depth',action='store_true')
    a=ap.parse_args()
    try:
        first=parse(a.log,a.start_frame)
        print(compare(first,parse(a.log2,a.start_frame)) if a.log2 else summary(first,a.full_depth),end='')
    except OSError as exc:ap.exit(1,f'{exc}\n')

if __name__=='__main__':main()
