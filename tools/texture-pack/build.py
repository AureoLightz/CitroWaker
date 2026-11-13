#!/usr/bin/env python3
"""Build the standalone PC texture converter with GCC or Clang; no SDK/game assets."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parent
SOURCES=['texconv.cpp','texfit.cpp','texmap.cpp','texpack.cpp','texture_support.cpp']
def build(output=None,cxx=None):
    if sys.byteorder!='little':raise RuntimeError('The current pack format requires a little-endian host.')
    compiler=cxx or os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
    if not compiler:raise RuntimeError('Install a C++17 GCC/Clang compiler (MinGW-w64 on Windows), or set CXX to its executable.')
    output=Path(output)if output else ROOT/'build'/('mktexpack.exe'if os.name=='nt'else'mktexpack')
    output=output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
    args=[compiler,'-std=c++17','-O2','-pthread','-I'+str(ROOT/'src'),str(ROOT/'mktexpack.cpp')]+[str(ROOT/'src'/s)for s in SOURCES]+['-o',str(output)]
    subprocess.run(args,check=True)
    return output
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);p.add_argument('--cxx');a=p.parse_args()
    try:print(build(a.output,a.cxx))
    except (RuntimeError,OSError,subprocess.CalledProcessError)as e:p.exit(1,str(e)+'\n')
