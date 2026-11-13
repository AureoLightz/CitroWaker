#!/usr/bin/env python3
"""Create a LOCAL setup manifest for an existing CIA and material bundles.
Does not clear a CIA for public distribution, download files or write an SD card.
"""
import argparse
import json
from pathlib import Path
import sys
import prepare

def create(cia,bundles,output,release='local build'):
    cia=prepare.checked(Path(cia));output=Path(output).expanduser().absolute()
    if output.exists():raise prepare.PreparationError('Manifest already exists; choose a new filename.')
    entries=[];names=set()
    import re
    for path in bundles:
        src=prepare.checked(Path(path));name=src.name
        if not re.fullmatch(r'ww_bundle[A-Za-z0-9_-]*\.bin',name)or name=='ww_bundle_misses.bin':
            raise prepare.PreparationError('Choose base/title material bundles named ww_bundle*.bin; do not distribute the growing misses file.')
        if name.casefold()in names:raise prepare.PreparationError('Duplicate bundle filename.')
        names.add(name.casefold());entries.append({'source':str(src),'name':name})
    if not entries:raise prepare.PreparationError('At least one locally prepared material bundle is required.')
    output.parent.mkdir(parents=True,exist_ok=True)
    data={'schema':1,'release':release,'cia':str(cia),'texture_tools':'bundled','threads':2,'material_bundles':entries}
    with output.open('x',encoding='utf-8')as f:json.dump(data,f,indent=2);f.write('\n')
    return output
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cia',required=True,type=Path);p.add_argument('--bundle',required=True,action='append',type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--label',default='local build');a=p.parse_args()
    try:print(create(a.cia,a.bundle,a.output,a.label))
    except (prepare.PreparationError,OSError)as e:p.exit(1,str(e)+'\n')
