"""CitroWaker SD export. Standard library only; never installs or downloads games."""
from __future__ import annotations
import argparse, hashlib, json, os, re, runpy, shutil, struct, subprocess, sys, tempfile, threading
from pathlib import Path

class PreparationError(Exception): pass
class Cancelled(PreparationError): pass

def checked(path: Path) -> Path:
    path=path.resolve(strict=True)
    if not path.is_file() or path.stat().st_size==0: raise PreparationError(f'Missing or empty file: {path}')
    return path

def validate_iso(path: Path):
    with path.open('rb') as f: header=f.read(0x440)
    if len(header)<0x440 or header[:6]!=b'GZLE01': raise PreparationError('Select the USA GameCube Wind Waker ISO (GZLE01). RVZ, WBFS and other regions are not supported.')
    if header[0x1c:0x20]!=bytes.fromhex('c2339f3d'): raise PreparationError('The input does not have a GameCube disc header.')
    if header[7]!=0: raise PreparationError('This build requires USA revision 0 (GZLE01). Other revisions are not supported.')
    return header[7]

def digest(path: Path, cancel: threading.Event, dest: Path|None=None):
    h=hashlib.sha256()
    with path.open('rb') as f:
        out=dest.open('xb') if dest else None
        try:
            while block:=f.read(4*1024*1024):
                if cancel.is_set(): raise Cancelled('Cancelled. No completed export was replaced.')
                h.update(block)
                if out: out.write(block)
        finally:
            if out: out.close()
    return h.hexdigest()

def extract_worker(extractor: str, parser: str, iso: str, output: str):
    # Upstream extractor's hardcoded development import is superseded by the
    # selected release's parser, without editing Claude's extractor source.
    sys.path.insert(0,str(Path(parser).parent))
    import importlib.util
    spec=importlib.util.spec_from_file_location('disc_manifest',parser)
    mod=importlib.util.module_from_spec(spec);sys.modules['disc_manifest']=mod;spec.loader.exec_module(mod)
    sys.argv=[extractor,iso,output]
    runpy.run_path(extractor,run_name='__main__')

def run_tool(args: list[str], cancel: threading.Event, log):
    log('Running '+Path(args[0]).name)
    # Disk-backed stdout avoids deadlock and never leaves an unbounded GUI queue.
    with tempfile.TemporaryFile(mode='w+b') as capture:
        proc=subprocess.Popen(args,stdout=capture,stderr=subprocess.STDOUT,shell=False)
        try:
            while proc.poll() is None:
                if cancel.wait(.1):
                    proc.terminate()
                    try: proc.wait(timeout=5)
                    except subprocess.TimeoutExpired: proc.kill();proc.wait()
                    raise Cancelled('Cancelled. Temporary files removed; existing exports unchanged.')
            capture.seek(0,2);capture.seek(max(0,capture.tell()-24000));tail=capture.read().decode('utf-8','replace');log(tail.strip())
            if proc.returncode: raise PreparationError(f'{Path(args[0]).name} failed ({proc.returncode}).\n{tail[-3000:]}')
        finally:
            if proc.poll() is None: proc.terminate();proc.wait()

def prepare(iso: Path, manifest: Path, destination: Path, cancel=None, log=print):
    cancel=cancel or threading.Event();iso=checked(iso);manifest=checked(manifest)
    config=json.loads(manifest.read_text(encoding='utf-8'))
    if config.get('schema')!=1: raise PreparationError('Unsupported release manifest schema.')
    root=manifest.parent
    def resource(key):
        value=config.get(key)
        if not isinstance(value,str) or not value: raise PreparationError(f'Release manifest is missing {key}.')
        return checked(root/value)
    cia=resource('cia')
    bundled=config.get('texture_tools')=='bundled'
    if bundled:
        tools=Path(__file__).resolve().parents[1]/'texture-pack'
        extractor=checked(tools/'extract_textures.py');parser=checked(tools/'third_party/switchwaker/disc_manifest.py')
        converter=tools/'build'/('mktexpack.exe' if os.name=='nt' else 'mktexpack')
    else:
        extractor=resource('extractor');parser=resource('disc_parser');converter=resource('texture_converter')
    bundles=config.get('material_bundles')
    if not isinstance(bundles,list) or not bundles: raise PreparationError('This release needs locally prepared material bundles. None were supplied.')
    inputs=[];names=set()
    for entry in bundles:
        src=checked(root/entry['source']);name=entry['name']
        if not isinstance(name,str) or not re.fullmatch(r'ww_bundle[A-Za-z0-9_-]*\.bin',name) or name=='ww_bundle_misses.bin':
            raise PreparationError('Unsafe material bundle filename in manifest.')
        if name.casefold() in names: raise PreparationError('Duplicate material bundle filename.')
        names.add(name.casefold());inputs.append((src,name))
    with cia.open('rb') as f:
        if f.read(4)!=b'\x20\x20\0\0': raise PreparationError('The selected release does not contain a recognised CIA header.')
    rev=validate_iso(iso);destination=destination.expanduser().absolute()
    if destination.exists(): raise PreparationError('Choose a NEW export folder. Existing folders and saves are never overwritten.')
    destination.parent.mkdir(parents=True,exist_ok=True)
    required=iso.stat().st_size+cia.stat().st_size+sum(p.stat().st_size for p,_ in inputs)+1024**3
    if shutil.disk_usage(destination.parent).free<required: raise PreparationError('Not enough output space: allow the ISO size plus at least 1 GiB for conversion and packaging.')
    threads=max(1,min(4,int(config.get('threads',2))))
    if bundled:
        build_script=tools/'build.py'
        # Rebuild when public source changes; avoid silently using a stale converter.
        sources=list((tools/'src').glob('*'))+[tools/'mktexpack.cpp',build_script]
        if not converter.is_file() or any(p.stat().st_mtime_ns>converter.stat().st_mtime_ns for p in sources):
            log('Building the bundled PC texture converter (C++17 compiler required)…')
            run_tool([sys.executable,str(build_script)],cancel,log)
        converter=checked(converter)
    with tempfile.TemporaryDirectory(prefix='.citrowaker-',dir=destination.parent) as work:
        stage=Path(work);data=stage/'3ds/ww3ds';data.mkdir(parents=True);(stage/'cia').mkdir();(data/'user').mkdir()
        log('Checking and copying your ISO…')
        iso_hash=digest(iso,cancel,data/'GZLE01.iso')
        # Use the staged immutable copy for the conversion, not a changing source.
        tool_log=[]
        def tool_report(s):tool_log.append(s);log(s)
        worker=([sys.executable] if getattr(sys,'frozen',False) else [sys.executable,str(Path(__file__).resolve())])+['--extract-worker',str(extractor),str(parser),str(data/'GZLE01.iso'),str(stage/'texsrc.bin')]
        run_tool(worker,cancel,tool_report)
        run_tool([str(converter),str(stage/'texsrc.bin'),str(data/'ww_texpack.bin'),'--threads',str(threads),'--verify'],cancel,tool_report)
        with (data/'ww_texpack.bin').open('rb') as f:
            magic,version,count,sig=struct.unpack('<4sIII',f.read(16))
        if magic!=b'WWTP' or version!=2 or count==0: raise PreparationError('The converter produced an incompatible or empty texture pack.')
        (stage/'texsrc.bin').unlink()
        log('Copying the CIA and material bundles…')
        records={'3ds/ww3ds/GZLE01.iso':iso_hash}
        for src,rel in [(cia,'cia/CitroWaker.cia')]+[(p,'3ds/ww3ds/'+n) for p,n in inputs]:
            records[rel]=digest(src,cancel,stage/rel)
        records['3ds/ww3ds/ww_texpack.bin']=digest(data/'ww_texpack.bin',cancel)
        report={'format':1,'release':str(config.get('release','unlabelled')),'disc_id':'GZLE01','disc_revision':rev,'texture_pack_version':version,'texture_entries':count,'texture_options_signature':sig,'sha256':records}
        (stage/'PREPARATION.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        (stage/'conversion.log').write_text('\n'.join(tool_log),encoding='utf-8')
        (stage/'INSTALL.txt').write_text('CitroWaker SD export\n\nCopy the cia/ and 3ds/ folders to your SD card. Install cia/CitroWaker.cia with your CIA installer, then launch from HOME Menu.\n\nFor an EXISTING installation: back up user/ first. Copy new program/data files; preserve user/, ww_bundle_misses.bin and your configuration. Never replace saves with an empty folder.\n\nYour game data was prepared locally. Do not upload this export to GitHub or redistribute it. DSP firmware, if required, must come from your console.\n\nThis tool did not install a title or test it on hardware. A texture pack requires a matching game build; read the release notes.\n',encoding='utf-8')
        if cancel.is_set(): raise Cancelled('Cancelled before export completion.')
        if destination.exists(): raise PreparationError('Output folder appeared during conversion; refusing to overwrite it.')
        # Same-parent staging: a failed conversion cannot leave a partial export.
        os.rename(stage,destination)
    log('Export complete: '+str(destination));return report

def main():
    if len(sys.argv)>1 and sys.argv[1]=='--extract-worker':
        extract_worker(*sys.argv[2:]);return
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('iso',type=Path);p.add_argument('--release',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
    try:prepare(a.iso,a.release,a.output)
    except (PreparationError,OSError,ValueError,KeyError) as e:p.exit(1,str(e)+'\n')
if __name__=='__main__':main()
