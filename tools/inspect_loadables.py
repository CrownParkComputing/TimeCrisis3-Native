#!/usr/bin/env python3
"""Compare pinned TC3LOAD file-backed bytes and recovered function ranges to RAM.

All output is metadata under .local; no guest payload is emitted or executed.
Function-map agreement is evidence for that map and snapshot, not completeness.
"""
import argparse
import csv
import hashlib
import json
import struct
import zipfile
from pathlib import Path
from inventory_tc3 import ROOT, FormatError, parse_elf


def digest(data):return hashlib.sha256(data).hexdigest()


def merge(ranges):
    result=[]
    for start,end in sorted(ranges):
        if result and start<=result[-1][1]:result[-1][1]=max(end,result[-1][1])
        else:result.append([start,end])
    return result


def compare(elf_path,state,map_path,others):
    elf=elf_path.read_bytes();header=parse_elf(elf)
    pin=json.loads((ROOT/'games/tc3/manifest.json').read_text())['bootstrap_sha256']
    if digest(elf)!=pin:raise FormatError('TC3LOAD hash does not match title manifest')
    if zipfile.is_zipfile(state):
        with zipfile.ZipFile(state) as z:
            entry=z.getinfo('eeMemory.bin')
            if not 32*1024**2<=entry.file_size<=128*1024**2:raise FormatError('Unexpected EE RAM size')
            ram=z.read(entry)
    else:ram=state.read_bytes()
    report={'schema_version':1,'elf_sha256':digest(elf),'state_path':str(state),'ram_sha256':digest(ram),'ram_size':len(ram),'segments':[],'other_payloads':[]}
    loads=[s for s in header['segments'] if s['type']==1 and s['file_size']]
    for s in loads:
        a=s['virtual_address'];n=s['file_size'];off=s['file_offset']
        if a+n>len(ram):raise FormatError('File-backed load outside RAM snapshot')
        original=elf[off:off+n];live=ram[a:a+n];changed=[i for i,(x,y) in enumerate(zip(original,live)) if x!=y]
        report['segments'].append({'address':a,'file_size':n,'memory_size':s['memory_size'],'file_sha256':digest(original),'ram_sha256':digest(live),'changed_bytes':len(changed),'first_changed_address':a+changed[0] if changed else None})
    ranges=[];rows=0;excluded=0
    with map_path.open() as f:
        for row in csv.DictReader(f):
            rows+=1;a=int(row['Start'],16);end=int(row['End'],16)
            if a>=end or not any(s['virtual_address']<=a<end<=s['virtual_address']+s['file_size'] for s in loads):excluded+=1;continue
            ranges.append((a,end))
    differences=[];total=0
    for a,end in merge(ranges):
        s=next(s for s in loads if s['virtual_address']<=a<end<=s['virtual_address']+s['file_size'])
        offset=s['file_offset']+a-s['virtual_address'];total+=end-a
        for delta,(x,y) in enumerate(zip(elf[offset:offset+end-a],ram[a:end])):
            if x!=y:differences.append(a+delta)
    report['recovered_functions']={'map_path':str(map_path),'map_sha256':digest(map_path.read_bytes()),'end_convention':'exclusive (ExportPS2Functions.java record.endExclusive)', 'rows':rows,'excluded_rows':excluded,'merged_ranges':merge(ranges),'unique_bytes':total,'changed_bytes':len(differences),'first_changed_addresses':differences[:32]}
    for path in others:
        b=path.read_bytes()
        report['other_payloads'].append({'path':str(path),'size':len(b),'sha256':digest(b),'contains_elf_magic':b'\x7fELF' in b,'whole_payload_ram_offset':ram.find(b) if b else None,'whole_payload_elf_offset':elf.find(b) if b else None,'leading_words':list(struct.unpack_from('<2I',b)) if len(b)>=8 else [],'load_format_validated':False})
    report['limitations']=['Agreement only applies to supplied snapshot and recovered function map.','A matching mapped range does not prove that unmapped/runtime-generated code never executes.','TC3GAME leading words are observations, not a validated load address or decompression size.','Absence of a whole-file match does not establish whether a packed payload is ever used.']
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--elf',required=True,type=Path);p.add_argument('--state',required=True,type=Path);p.add_argument('--function-map',required=True,type=Path);p.add_argument('--payload',action='append',default=[],type=Path);p.add_argument('--output',type=Path,default=ROOT/'.local/inventory/loadables.json');a=p.parse_args()
    if not a.output.resolve().is_relative_to((ROOT/'.local').resolve()):p.error('Outputs must remain under project .local')
    report=compare(a.elf,a.state,a.function_map,a.payload);a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'output':str(a.output),'mapped_code':{k:v for k,v in report['recovered_functions'].items() if k!='merged_ranges'}}))

if __name__=='__main__':main()
