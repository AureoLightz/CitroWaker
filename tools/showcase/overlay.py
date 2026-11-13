#!/usr/bin/env python3
"""Annotate real footage with measured timing CSV; never infer FPS from video FPS."""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

FRAME = re.compile(r'^\[frame\] (\d+) .*? present ([\d.]+) tick (\d+)\b')
TICK_HZ = 268123480


def read_timings(path):
    with Path(path).open(newline='', encoding='utf-8-sig') as f:
        rows = [(float(r['time_s']), float(r['frame_ms'])) for r in csv.DictReader(f)]
    if not rows or any(not math.isfinite(t) or not math.isfinite(ms) or t < 0 or ms <= 0 for t, ms in rows):
        raise ValueError('Timing rows need finite time_s >= 0 and frame_ms > 0')
    if any(b[0] <= a[0] for a, b in zip(rows, rows[1:])):
        raise ValueError('Timing timestamps must strictly increase')
    return rows


def export_log(log, out):
    rows = []
    for line in Path(log).read_text(errors='replace').splitlines():
        m = FRAME.match(line)
        if m:
            rows.append((int(m[1]), float(m[2]), int(m[3])))
    if not rows:
        raise ValueError('No per-frame [frame] present/tick records; 60-frame averages are not accepted')
    if any(b[0] != a[0] + 1 or b[2] <= a[2] for a, b in zip(rows, rows[1:])):
        raise ValueError('Log needs one continuous frame range, without missing/duplicate/reset frames')
    if Path(out).exists():
        raise FileExistsError(out)
    with Path(out).open('x', newline='') as f:
        w = csv.writer(f)
        w.writerow(['time_s', 'frame_ms'])
        for _, ms, tick in rows:
            w.writerow([f'{(tick-rows[0][2])/TICK_HZ:.9f}', ms])
    return len(rows)


def ass_time(t):
    n = max(0, round(t * 100))
    return f'{n//360000}:{n//6000%60:02}:{n//100%60:02}.{n%100:02}'


def safe_text(s):
    return s.replace('\\', '/').replace('{', '(').replace('}', ')').replace('\n', ' ')


def make_ass(rows, width, height, duration, title, metric, offset=0, scale=1, max_ms=500):
    # CSV timestamps are synced to footage by the caller. Values are NEVER scaled.
    rows = [(t*scale+offset, ms) for t, ms in rows]
    times = [r[0] for r in rows]
    total_h = height + 148
    result = [f'''[Script Info]
ScriptType: v4.00+
PlayResX: {width}
PlayResY: {total_h}
WrapStyle: 2
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,DejaVu Sans,18,&H00EEF3EE,&H000000FF,&H00132219,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text''']

    def event(layer, start, end, text):
        result.append(f'Dialogue: {layer},{ass_time(start)},{ass_time(end)},Default,,0,0,0,,{text}')

    event(1, 0, duration, f'{{\\pos(16,{height+10})\\b1}}{safe_text(title)}')
    event(1, 0, duration, f'{{\\pos(16,{height+38})\\fs13}}{metric.upper()} timing | 1 s rolling FPS | graph 0–{max_ms:g} ms')
    left, right, top, bottom = 16, width-16, height+66, height+112
    target_y = bottom - min(1, (1000/30)/max_ms)*(bottom-top)
    event(0, 0, duration, f'{{\\p1\\c&H607058&}}m {left} {target_y:.1f} l {right} {target_y:.1f}{{\\p0}}')
    for step in range(math.ceil(duration*10)):
        t = step/10
        end = min(duration, t+.1)
        i = bisect.bisect_right(times, t)-1
        if i < 0 or t-times[i] >= 1-1e-9:
            label = 'No timing sample'
        else:
            recent = rows[bisect.bisect_left(times, t-1):i+1]
            fps = 1000*len(recent)/sum(ms for _, ms in recent)
            label = f'{fps:.1f} FPS | {rows[i][1]:.1f} ms'
        event(1, t, end, f'{{\\pos({width-16},{height+10})\\an9\\b1}}{label}')
        history = rows[max(0, bisect.bisect_left(times, t-6)):i+1]
        points = []
        for at, ms in history:
            x = left+(at-(t-6))/6*(right-left)
            y = bottom-min(1, ms/max_ms)*(bottom-top)
            points.append(f'{x:.1f} {y:.1f}')
        if len(points)>1:
            event(0, t, end, '{\\p1\\c&H79DFC3&\\bord1\\shad0}m '+points[0]+' l '+' '.join(points[1:])+'{\\p0}')
    return '\n'.join(result)+'\n'


def render(args):
    video, out = Path(args.video).resolve(), Path(args.output).resolve()
    if out.exists():
        raise FileExistsError(out)
    if not video.is_file() or out.suffix.lower() != '.mp4':
        raise ValueError('Supply an existing video and a new .mp4 output')
    rows = read_timings(args.timings)
    if not all(math.isfinite(x) for x in [args.offset,args.time_scale,args.max_ms]) or not 400 <= args.width <= 3840 or args.width % 2 or args.time_scale <= 0 or args.max_ms <= 0:
        raise ValueError('Width must be even and >=400; scale and graph range must be positive')
    ffmpeg, ffprobe = shutil.which('ffmpeg'), shutil.which('ffprobe')
    if not ffmpeg or not ffprobe:
        raise RuntimeError('Install FFmpeg with libass support (ffmpeg and ffprobe)')
    info = json.loads(subprocess.check_output([ffprobe, '-v', 'error', '-show_streams', '-show_format', '-of', 'json', str(video)], text=True))
    v = next(s for s in info['streams'] if s['codec_type']=='video')
    duration = float(info['format']['duration'])
    if rows[-1][0]*args.time_scale+args.offset < 0 or rows[0][0]*args.time_scale+args.offset > duration:
        raise ValueError('Timings do not overlap the video; check synchronization/offset')
    height = 2*round(args.width*int(v['height'])/int(v['width'])/2)
    with tempfile.TemporaryDirectory(prefix='citrowaker-overlay-') as temp:
        temp = Path(temp)
        (temp/'overlay.ass').write_text(make_ass(rows, args.width, height, duration, args.title, args.metric, args.offset, args.time_scale, args.max_ms), encoding='utf-8')
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin', '-n', '-i', str(video), '-vf', f'scale={args.width}:{height}:flags=neighbor,pad=iw:ih+148:0:0:color=0x102c43,ass=overlay.ass', '-map', '0:v:0', '-map', '0:a?', '-c:v', 'libx264', '-threads', '2', '-crf', '18', '-preset', 'medium', '-c:a', 'aac', '-b:a', '192k', '-movflags', '+faststart', str(temp/'encoded.mp4')]
        subprocess.run(cmd, cwd=temp, check=True)
        # Exclusive destination creation; cancellation/encode failure leaves no partial output.
        created = False
        try:
            with out.open('xb') as dst, (temp/'encoded.mp4').open('rb') as src:
                created = True
                shutil.copyfileobj(src, dst)
        except BaseException:
            if created:
                out.unlink(missing_ok=True)
            raise
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    e = sub.add_parser('export-log', help='Export console-clock timestamps; NOT host-video synchronization')
    e.add_argument('log'); e.add_argument('output')
    r = sub.add_parser('render')
    r.add_argument('video');r.add_argument('timings');r.add_argument('output')
    r.add_argument('--metric', required=True, choices=['virtual','console','host'])
    r.add_argument('--title', default='CitroWaker — Ganondorf')
    r.add_argument('--width', type=int, default=800)
    r.add_argument('--offset', type=float, default=0, help='Video timestamp corresponding to CSV t=0')
    r.add_argument('--time-scale', type=float, default=1, help='Video seconds per CSV second; requires calibrated sync')
    r.add_argument('--max-ms', type=float, default=500)
    a = p.parse_args()
    try:
        if a.command=='export-log':
            print(f'Exported {export_log(a.log,a.output)} samples. Timestamps use the console clock; align to footage before rendering. Emulator timing is VIRTUAL, not host FPS.')
        else: print(render(a))
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError, StopIteration) as exc:
        p.exit(1, f'Error: {exc}\n')

if __name__=='__main__':main()
