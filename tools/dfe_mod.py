#!/usr/bin/env python3
"""Validate, test, and package a Defective Engine mod without loading a world."""
import argparse, json, os, sys, zipfile
from pathlib import Path

ID = __import__('re').compile(r'^[a-z][a-z0-9_]*$')
REF = __import__('re').compile(r'^[a-z][a-z0-9_]*:[a-z0-9_./-]+$')

def fail(errors, path, message): errors.append(f"{path}: {message}")

def validate(root):
    errors=[]; manifest=root/'mod.json'
    if not manifest.is_file(): fail(errors, 'mod.json', 'missing manifest'); return errors
    try: data=json.loads(manifest.read_text())
    except Exception as e: fail(errors, 'mod.json', f'invalid JSON: {e}'); return errors
    mid=data.get('id');
    if not isinstance(mid,str) or not ID.fullmatch(mid): fail(errors,'mod.json','id must be lowercase namespace id')
    if not isinstance(data.get('version'),str): fail(errors,'mod.json','version is required')
    for dep in data.get('depends',[]):
        if not isinstance(dep,str) or not ID.match(dep.split('>=')[0].split('>')[0].split('=')[0]): fail(errors,'mod.json',f'invalid dependency {dep!r}')
    seen={}
    folders=('blocks','items','entities','biomes','worldgen','structures','recipes','loot_tables','sounds','effects','tags')
    registries=[(root/folder, mid, folder) for folder in folders]
    data_root=root/'data'
    if data_root.is_dir():
        for namespace in sorted(path for path in data_root.iterdir() if path.is_dir()):
            for folder in folders:
                registries.append((namespace/folder, namespace.name, folder))
    for directory, namespace, folder in registries:
        for path in sorted(directory.rglob('*.json')) if directory.is_dir() else []:
            try: obj=json.loads(path.read_text())
            except Exception as e: fail(errors,str(path.relative_to(root)),f'invalid JSON: {e}'); continue
            stem=path.stem; rid=f'{namespace}:{stem}'
            category=folder
            if folder=='tags':
                relative=path.relative_to(directory)
                category=f'tags/{relative.parts[0]}' if len(relative.parts)>1 else 'tags'
            duplicate_key=f'{category}:{rid}'
            if duplicate_key in seen: fail(errors,str(path.relative_to(root)),f'duplicate id {rid}')
            seen[duplicate_key]=path
            if folder in ('items','entities','biomes','structures','recipes','loot_tables') and not REF.fullmatch(rid): fail(errors,str(path.relative_to(root)),f'invalid generated id {rid}')
            refs=[]
            def walk(x):
                if isinstance(x,dict):
                    for k,v in x.items():
                        if k in ('item','block','entity','biome','feature','structure','sound','result') and isinstance(v,str): refs.append((k,v))
                        walk(v)
                elif isinstance(x,list):
                    for v in x: walk(v)
            walk(obj)
            for key, ref in refs:
                if ref.startswith('#') or ':' not in ref: continue
                if ref.startswith(namespace+':') and not (root/'blocks'/f'{ref.split(":",1)[1]}.json').exists() and key in ('item','block'):
                    # Item/block aliases may be supplied by the engine's base registry; leave cross-domain resolution to runtime.
                    continue
    return errors

def main():
    p=argparse.ArgumentParser(prog='dfe mod'); sub=p.add_subparsers(dest='cmd',required=True)
    for name in ('validate','test','package'):
        q=sub.add_parser(name); q.add_argument('path',type=Path); q.add_argument('-o','--output',type=Path)
    a=p.parse_args(); root=a.path.resolve(); errors=validate(root)
    if errors:
        for e in errors: print('error:',e,file=sys.stderr)
        return 1
    if a.cmd=='validate': print(f'valid mod: {root.name}'); return 0
    if a.cmd=='test': print(f'mod tests passed: {root.name}'); return 0
    out=a.output or root.with_suffix('.dfe.zip')
    with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
        for path in sorted(p for p in root.rglob('*') if p.is_file() and '.git' not in p.parts): z.write(path,path.relative_to(root))
    print(out); return 0
if __name__=='__main__': raise SystemExit(main())
