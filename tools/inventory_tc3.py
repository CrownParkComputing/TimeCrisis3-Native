#!/usr/bin/env python3
"""Inventory explicit private inputs; ELF discovery is not proof of full code coverage."""
import argparse
import hashlib
import json
import mmap
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAX_HEADERS = 16384

class FormatError(ValueError):
    pass


def checked_span(size, offset, length, label):
    if offset < 0 or length < 0 or offset > size or length > size-offset:
        raise FormatError(f'{label} outside input')


def parse_elf(data, base=0):
    """Strict bounded ELF32/MIPS metadata parser; offsets relative to ELF start."""
    size=len(data)-base
    checked_span(len(data),base,52,'ELF header')
    ident=bytes(data[base:base+16])
    if ident[:4]!=b'\x7fELF':raise FormatError('Not ELF')
    if ident[4:7]!=b'\x01\x01\x01':raise FormatError('Only ELF32 little-endian version 1 supported')
    typ,machine,version,entry,phoff,shoff,flags,ehsize,phsize,phnum,shsize,shnum,shstr=struct.unpack_from('<HHIIIIIHHHHHH',data,base+16)
    if machine!=8 or version!=1:raise FormatError('Not MIPS ELF version 1')
    if ehsize!=52:raise FormatError('Unexpected ELF header size')
    if phnum==65535 or (shnum==0 and shoff) or shstr==65535:raise FormatError('Extended ELF numbering unsupported')
    if phnum>MAX_HEADERS or shnum>MAX_HEADERS:raise FormatError('Too many ELF headers')
    if phnum and (phsize!=32 or phoff<52):raise FormatError('Invalid program header table')
    if shnum and (shsize!=40 or shoff<52):raise FormatError('Invalid section header table')
    if shstr and (not shnum or shstr>=shnum):raise FormatError('Invalid section string table index')
    checked_span(size,phoff,phnum*phsize,'Program headers')
    checked_span(size,shoff,shnum*shsize,'Section headers')
    segments=[];sections=[];warnings=[]
    for i in range(phnum):
        kind,offset,vaddr,paddr,filesz,memsz,pflags,align=struct.unpack_from('<8I',data,base+phoff+i*phsize)
        checked_span(size,offset,filesz,'Segment payload')
        if kind==1:
            if filesz>memsz:raise FormatError('PT_LOAD filesz exceeds memsz')
            if vaddr+memsz>2**32:raise FormatError('PT_LOAD address wraps')
            if align>1 and (align & (align-1) or vaddr%align!=offset%align):raise FormatError('Invalid PT_LOAD alignment')
        segments.append(dict(type=kind,file_offset=offset,virtual_address=vaddr,physical_address=paddr,file_size=filesz,memory_size=memsz,flags=pflags,alignment=align))
    for i in range(shnum):
        name,kind,sflags,addr,offset,length,link,info,align,entsize=struct.unpack_from('<10I',data,base+shoff+i*shsize)
        if kind!=8:checked_span(size,offset,length,'Section payload')
        if addr+length>2**32:raise FormatError('Section address wraps')
        if align>1 and align & (align-1):raise FormatError('Invalid section alignment')
        sections.append(dict(index=i,type=kind,flags=sflags,address=addr,size=length,file_offset=offset))
    loads=[s for s in segments if s['type']==1]
    executable=[s for s in loads if s['flags']&1]
    for i,a in enumerate(loads):
        for b in loads[i+1:]:
            if max(a['virtual_address'],b['virtual_address'])<min(a['virtual_address']+a['memory_size'],b['virtual_address']+b['memory_size']):warnings.append('Overlapping PT_LOAD ranges')
    if typ==2 and not any(s['virtual_address']<=entry<s['virtual_address']+s['memory_size'] for s in executable):warnings.append('Executable entry outside executable PT_LOAD')
    return dict(offset=base,type=typ,machine=machine,entry=entry,flags=flags,segments=segments,executable_sections=[s for s in sections if s['flags']&4],warnings=warnings,classification='mips_irx' if typ in (0xff80,0xff91) else 'mips_executable' if typ==2 else 'mips_relocatable' if typ==1 else 'mips_elf')


def scan_file(path, hash_payload=True):
    record={'path':str(path),'size':path.stat().st_size,'images':[],'rejected_elf_candidates':[]}
    if hash_payload:
        with path.open('rb') as f:record['sha256']=hashlib.file_digest(f,'sha256').hexdigest()
    if not record['size']:record['classification']='empty';return record
    with path.open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        start=0
        while len(record['images'])+len(record['rejected_elf_candidates'])<4096:
            offset=data.find(b'\x7fELF',start)
            if offset<0:break
            try:record['images'].append(parse_elf(data,offset))
            except FormatError as e:record['rejected_elf_candidates'].append({'offset':offset,'reason':str(e)})
            start=offset+4
        else:record['scan_limit_reached']=True
        record['classification']='elf_container' if record['images'] else 'unrecognized_payload'
        if record['size']>=8:
            # Observations only: these words cannot establish a load-address contract.
            record['leading_words']=list(struct.unpack_from('<2I',data))
    return record


def iso_directory(path):
    """Bounded ISO9660 primary directory traversal; no archive extraction."""
    size=path.stat().st_size
    with path.open('rb') as f:
        def read(offset,length):
            checked_span(size,offset,length,'ISO extent');f.seek(offset);b=f.read(length)
            if len(b)!=length:raise FormatError('Short ISO read')
            return b
        pvd=None
        for sector in range(16,80):
            b=read(sector*2048,2048)
            if b[1:7]!=b'CD001\x01':raise FormatError('Invalid ISO volume descriptor')
            if b[0]==1:pvd=b;break
            if b[0]==255:break
        if pvd is None:raise FormatError('No ISO primary volume descriptor')
        def both(record,offset):
            a=struct.unpack_from('<I',record,offset)[0];b=struct.unpack_from('>I',record,offset+4)[0]
            if a!=b:raise FormatError('ISO endian copies differ')
            return a
        block=struct.unpack_from('<H',pvd,128)[0]
        if block!=2048 or struct.unpack_from('>H',pvd,130)[0]!=block:raise FormatError('Unsupported ISO block size')
        root=pvd[156:190];pending=[('',both(root,2),both(root,10))];seen=set();entries=[]
        while pending:
            directory,lba,length=pending.pop()
            if (lba,length) in seen:continue
            seen.add((lba,length))
            if len(seen)>4096 or length>16*1024*1024:raise FormatError('ISO directory limit')
            data=read(lba*block,length);off=0
            while off<len(data):
                n=data[off]
                if not n:off=(off//block+1)*block;continue
                if n<34 or off+n>len(data) or off//block!=(off+n-1)//block:raise FormatError('Invalid ISO directory record')
                rec=data[off:off+n];off+=n
                if 33+rec[32]>n:raise FormatError('Invalid ISO name')
                name=rec[33:33+rec[32]]
                if name in (b'\0',b'\1'):continue
                name=name.decode('ascii','replace').split(';')[0];loc=both(rec,2);count=both(rec,10)
                checked_span(size,loc*block,count,'ISO file')
                item={'path':directory+'/'+name,'offset':loc*block,'size':count,'directory':bool(rec[25]&2),'multi_extent':bool(rec[25]&128)}
                entries.append(item)
                if len(entries)>100000:raise FormatError('ISO entry limit')
                if item['directory']:pending.append((item['path'],loc,count))
                elif count>=4:
                    prefix=read(loc*block,min(count,52))
                    item['elf_magic_at_start']=prefix[:4]==b'\x7fELF'
        return {'volume':pvd[40:72].decode('ascii','replace').strip(),'entries':entries,'archive_payloads_decoded':False}


def inventory(roots, dvd=None, hash_dvd=False):
    manifest=json.loads((ROOT/'games/tc3/manifest.json').read_text())
    paths=set();missing=[]
    for root in roots:
        if not root.exists():missing.append(str(root));continue
        for p in ([root] if root.is_file() else root.rglob('*')):
            if p.is_file() and not p.is_symlink():paths.add(p.resolve())
    records=[scan_file(p) for p in sorted(paths)]
    pinned=[r['path'] for r in records if r.get('sha256')==manifest['bootstrap_sha256'] and any(i['offset']==0 and i['type']==2 for i in r['images'])]
    report={'schema_version':1,'title':manifest['title'],'bootstrap_verified':bool(pinned),'pinned_bootstrap_paths':pinned,'inputs':records,'missing_inputs':missing,'coverage':{'native_validated_images':0,'complete_native_coverage':False,'limitations':['Static ELF inventory does not discover all compressed, encrypted, relocated, or runtime-generated code.','Unrecognized payloads require format recovery or runtime load/write observations.','DVD directory listing does not decode FGH/FGI archive contents.', 'Executable PT_LOAD zero-fill ranges are writable memory reservations, not inventoried instruction bytes.', 'IRX modules may relocate at runtime; recorded addresses do not establish final executable coverage.','An embedded ELF is a candidate; container and extracted-image identities must be pinned separately.']}}
    if dvd:
        if not dvd.exists():report['missing_inputs'].append(str(dvd))
        else:
            report['dvd']={'path':str(dvd),'size':dvd.stat().st_size,**iso_directory(dvd)}
            if hash_dvd:
                with dvd.open('rb') as f:report['dvd']['sha256']=hashlib.file_digest(f,'sha256').hexdigest()
    else:report['coverage']['limitations'].append('No DVD image supplied.')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('inputs',nargs='+',type=Path);p.add_argument('--dvd',type=Path);p.add_argument('--hash-dvd',action='store_true');p.add_argument('--output',type=Path,default=ROOT/'.local/inventory/tc3.json');a=p.parse_args()
    output=a.output.resolve()
    if not output.is_relative_to((ROOT/'.local').resolve()):p.error('Inventory outputs must remain under this project .local directory')
    report=inventory(a.inputs,a.dvd,a.hash_dvd);output.parent.mkdir(parents=True,exist_ok=True);output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'output':str(output),'files':len(report['inputs']),'elf_images':sum(len(r['images']) for r in report['inputs']),'bootstrap_verified':report['bootstrap_verified'],'complete_native_coverage':False,'missing_inputs':report['missing_inputs']}))
    return 0 if report['bootstrap_verified'] and not report['missing_inputs'] else 1

if __name__=='__main__':raise SystemExit(main())
