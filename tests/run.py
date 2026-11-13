#!/usr/bin/env python3
"""Run public host tests without any game assets, SDK or emulator."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys

ROOT=Path(__file__).resolve().parents[1]
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--python-only',action='store_true',help='Skip C++ tests (requires no compiler)')
    args=parser.parse_args()
    for folder in ['tools/setup','tools/console-report','tools/showcase']:
        subprocess.run([sys.executable,'-m','unittest','discover','-v','-s',str(ROOT/folder)],check=True,cwd=ROOT)
    if not args.python_only:
        compiler=os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
        if not compiler:parser.error('A C++17 compiler is required, or use --python-only.')
        with tempfile.TemporaryDirectory(prefix='citrowaker-test-') as directory:
            binary=Path(directory)/('platform.exe' if os.name=='nt' else 'platform')
            subprocess.run([compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'platform/n3ds'),str(ROOT/'tests/platform.cpp'),'-o',str(binary)],check=True)
            subprocess.run([str(binary),str(Path(directory)/'qol.ini')],check=True)
if __name__=='__main__':main()
