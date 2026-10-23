#!/usr/bin/env python3
"""Prepare isolated TC3 playtest storage from explicitly supplied private reference assets.

The frontend's --data-root takes the parent of PCSX2x6/, matching upstream
EmuFolders::SetDataDirectory. Existing user SRAM/cards/settings are preserved.
"""
import argparse
import configparser
import hashlib
import json
import os
import shutil
import struct
import tempfile
import zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
DEFAULT_REFERENCE=ROOT/'.local/reference'
PINS={
    'games/proverb.elf':'268ab240d617da3aa1a1238fd8829b9af1ec0aaf404dd804b39f3b53f495e865',
    'games/TST1.iso':'647519122f0690bf362fc99bbb46008cbf5cae62c98f9021b8d3a85cb5e712ac',
    'PCSX2x6/bios/246C.bin':'8eb8f4d963dde4659de4fbb6eca870077f9af59a9100e8eecd59ec836ff79fae',
    'PCSX2x6/memcards/NM00012.ps2':'15787a204eeecc39897f4e0395120729f69661ceb85251a02c52c06577cef13c',
    'reference-calibrated-stage1-start.p2s':'9906063eab6275746deac3064b99508b8a8a919aa464a60be8e35ade0494d67c',
}
CALIBRATION=(320,220,90,360,550,360,550,80,90,80)


def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()


def ini(path):
    c=configparser.ConfigParser(interpolation=None);c.optionxform=str
    with path.open() as f:c.read_file(f)
    return c


def text_ini(c):
    import io
    out=io.StringIO();c.write(out);return out.getvalue()


def atomic_copy(source,dest):
    dest.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix='.prepare-',dir=dest.parent)
    os.close(fd)
    try:
        shutil.copyfile(source,name)
        # No shared inode for mutable data; explicit user ownership/mode.
        os.chmod(name,0o600)
        os.replace(name,dest)
    finally:
        if os.path.exists(name):os.unlink(name)


def write_new(path,text):
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('x') as f:f.write(text)


def prepare(reference,destination,with_state=False):
    reference=reference.resolve();destination=destination.resolve()
    if not destination.is_relative_to((ROOT/'.local').resolve()):raise ValueError('Destination must be inside new project .local')
    if destination==reference or destination.is_relative_to(reference) or reference.is_relative_to(destination):raise ValueError('Reference and playtest storage must be separate')
    required=list(PINS if with_state else (k for k in PINS if not k.endswith('.p2s')))
    required+=['games/sram.bin','games/NM00012.acgame','PCSX2x6/inis/PCSX2.ini','PCSX2x6/bios/246C.nvm','PCSX2x6/bios/246C.mec']
    missing=[str(reference/p) for p in required if not (reference/p).is_file()]
    if missing:raise ValueError('Missing private prerequisites: '+', '.join(missing))
    hashes={p:sha(reference/p) for p in required}
    for p,expected in PINS.items():
        if p in hashes and hashes[p]!=expected:raise ValueError(f'Private input hash mismatch: {p}')
    sram=(reference/'games/sram.bin').read_bytes()
    if len(sram)!=32768 or struct.unpack_from('<10H',sram,0xd8)!=CALIBRATION:raise ValueError('Reference SRAM does not have the verified five-point gun calibration')
    ac=ini(reference/'games/NM00012.acgame')
    if ac.get('game','gameid')!='NM00012' or ac.get('game','platform')!='246':raise ValueError('Reference arcade manifest is not TC3 System246')
    config=ini(reference/'PCSX2x6/inis/PCSX2.ini')
    if with_state:
        with zipfile.ZipFile(reference/'reference-calibrated-stage1-start.p2s') as z:
            if z.getinfo('eeMemory.bin').file_size!=128*1024**2:raise ValueError('Unexpected reference savestate RAM size')
            state_sram=z.read('ACSRAM.bin')
            if struct.unpack_from('<10H',state_sram,0xd8)!=CALIBRATION:raise ValueError('Savestate contains obsolete gun calibration')
    # Validate all existing target paths before creating anything. Directory
    # symlinks could otherwise redirect writes back into reference storage.
    for part in [destination,*destination.parents]:
        if part.is_symlink():raise ValueError(f'Destination ancestor is a symlink: {part}')
    if destination.exists():
        for p in destination.rglob('*'):
            if p.is_symlink() and p!=destination/'games/TST1.iso':raise ValueError(f'Unexpected symlink in playtest storage: {p}')
    copied=[];preserved=[]
    source_files=['games/proverb.elf','games/sram.bin','PCSX2x6/bios/246C.bin','PCSX2x6/bios/246C.nvm','PCSX2x6/bios/246C.mec','PCSX2x6/memcards/NM00012.ps2']
    if with_state:source_files.append('reference-calibrated-stage1-start.p2s')
    def target(name):
        return destination/('states/stage1-start.p2s' if name=='reference-calibrated-stage1-start.p2s' else name)
    for name in source_files:
        src=reference/name;dst=target(name)
        if dst.exists():
            if not dst.is_file() or os.path.samefile(src,dst):raise ValueError(f'Existing playtest file is not an independent copy: {dst}')
            if name in ('games/proverb.elf','PCSX2x6/bios/246C.bin','reference-calibrated-stage1-start.p2s') and sha(dst)!=hashes[name]:raise ValueError(f'Existing immutable playtest asset has wrong hash: {dst}')
    existing_ini=destination/'PCSX2x6/inis/PCSX2.ini'
    if existing_ini.exists():
        old=ini(existing_ini)
        if old.get('EmuCore/GS','Renderer',fallback='')!='14':raise ValueError('Existing playtest settings do not select Vulkan (Renderer=14); preserved without modification')
    media=destination/'games/TST1.iso'
    if media.is_symlink() and media.resolve()!=(reference/'games/TST1.iso').resolve():raise ValueError('Existing DVD link points to another input')
    if media.exists() and not media.is_symlink() and sha(media)!=PINS['games/TST1.iso']:raise ValueError('Existing DVD has wrong hash')
    manifest=destination/'games/NM00012.acgame'
    expected_data={'subdir':'','elf':'proverb.elf','dongle':'NM00012.ps2','mediasrc':'TST1.iso','media':'DVD','sram':'sram.bin'}
    if manifest.exists():
        current=ini(manifest)
        if current.get('game','gameid')!='NM00012' or any(current.get('data',k,fallback=None)!=v for k,v in expected_data.items()):raise ValueError('Existing arcade manifest does not use isolated TC3 assets; preserved without modification')
    for name in source_files:
        dst=target(name)
        if dst.exists():preserved.append(name)
        else:atomic_copy(reference/name,dst);copied.append(name)
    media=destination/'games/TST1.iso'
    if media.is_symlink():
        if media.resolve()!=(reference/'games/TST1.iso').resolve():raise ValueError('Existing DVD link points to another input')
    elif media.exists():
        if sha(media)!=PINS['games/TST1.iso']:raise ValueError('Existing DVD has wrong hash')
    else:media.symlink_to(reference/'games/TST1.iso')
    # Preserve pre-existing settings; enforce setup values only at first creation.
    settings={
        'UI':{'SetupWizardIncomplete':'false','ConfirmShutdown':'false','PauseOnFocusLoss':'false'},
        'Filenames':{'BIOS':'246C.bin'},
        'EmuCore/GS':{'Renderer':'14','upscale_multiplier':'1','AspectRatio':'4:3'},
        'EmuCore/Speedhacks':{'EECycleRate':'0','EECycleSkip':'0'},
        'USB1':{'Type':'guncon2','guncon2_Pointer':'Pointer-0','guncon2_Trigger':'Pointer-0/LeftButton','guncon2_A':'Keyboard/Space','guncon2_Start':'Keyboard/Return'},
        'JVS':{'ToggleTestMode':'Keyboard/F9','Coin1':'Keyboard/5','P1_Start':'Keyboard/Return','P1_Service':'Keyboard/9','P1_Up':'Keyboard/Up','P1_Down':'Keyboard/Down','P1_Button1':'Keyboard/E'},
        'InputSources':{'SDL':'false','Evdev':'false'},
        'MemoryCards':{'Slot1_Filename':'NM00012.ps2','Slot2_Filename':''},
        'Hotkeys':{'TogglePause':'Keyboard/Pause'},
    }
    for section,values in settings.items():
        if not config.has_section(section):config.add_section(section)
        for key,value in values.items():config.set(section,key,value)
    # Avoid inheriting a reference-machine-specific evdev mouse selection.
    config.remove_section('Evdev')
    # Generic JVS Button2..6 are not TC3's pedal; USB GunCon2 A is authoritative.
    for key in ('P1_Button2','P1_Button6'):config.remove_option('JVS',key)
    if not existing_ini.exists():write_new(existing_ini,text_ini(config))
    ac.set('game','name','Time Crisis 3 TST3 playtest')
    for key,value in expected_data.items():ac.set('data',key,value)
    manifest=destination/'games/NM00012.acgame'
    if not manifest.exists():write_new(manifest,text_ini(ac))
    for name in ('sstates','logs','cache','snaps'):(destination/'PCSX2x6'/name).mkdir(parents=True,exist_ok=True)
    report={'schema_version':1,'data_root':str(destination),'core_data_directory':str(destination/'PCSX2x6'),'game':str(manifest),'settings':str(existing_ini),'calibrated_start_state':str(destination/'states/stage1-start.p2s') if with_state else None,'private_input_sha256':hashes,'copied':copied,'preserved':preserved,'dvd_storage':'read-only media symlink; core DVD access must remain read-only','mutable_storage':'independent copies, preserved on rerun','controls':{'coin':'5','start':'Return','aim':'mouse','trigger':'left mouse button','pedal':'Space','cabinet_test':'F9','cabinet_service':'9','cabinet_enter':'E','frontend_menu':'F10'}}
    (destination/'preparation.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--reference',type=Path,default=DEFAULT_REFERENCE);p.add_argument('--destination',type=Path,default=ROOT/'.local/playtest');state=p.add_mutually_exclusive_group();state.add_argument('--with-state',action='store_true',help='Copy the legacy research state (not supported for gameplay boot)');state.add_argument('--no-state',action='store_true',help='Normal boot (the default)');a=p.parse_args()
    try:r=prepare(a.reference,a.destination,a.with_state)
    except (OSError,ValueError,KeyError,configparser.Error,zipfile.BadZipFile) as e:p.exit(1,f'Playtest preparation failed: {e}\n')
    print(json.dumps({k:r[k] for k in ('data_root','core_data_directory','game','settings','calibrated_start_state','copied','preserved','controls')},indent=2))

if __name__=='__main__':main()
