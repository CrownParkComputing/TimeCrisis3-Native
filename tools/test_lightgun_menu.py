#!/usr/bin/env python3
"""Live border/maintenance regression on a fresh diagnostic cabinet session.

Requires Hyprland, XWayland/xdotool, grim, Pillow and development symbols.
Reads host globals only; sends real original test-switch input. Restores border
settings and the test switch, resumes the guest, and leaves its windows open.
The session report must have workers with pid/log/data_root (TC3 pair format).
"""
import argparse
import configparser
import json
from pathlib import Path
import re
import struct
import subprocess
import time
from PIL import Image
from sample_guest_progress import Reader
from test_cabinet_ui import wait_for

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,default=ROOT/'.local/cabinet-pair-run.json')
    parser.add_argument('--evidence',type=Path,default=ROOT/'.local/lightgun-menu')
    args=parser.parse_args()
    workers=json.loads(args.session.read_text())['workers']
    primary=next(iter(workers))
    binary=Path(f"/proc/{workers[primary]['pid']}/exe").resolve()
    symbols={}
    for line in subprocess.check_output(['nm','-C','--defined-only',str(binary)],text=True).splitlines():
        parts=line.split(maxsplit=2)
        if len(parts)==3: symbols[parts[2]]=int(parts[0],16)
    readers={role:Reader(w['pid'],binary,symbols) for role,w in workers.items()}
    args.evidence.mkdir(parents=True,exist_ok=True)

    def client(role):
        clients=json.loads(subprocess.check_output(['hyprctl','clients','-j'],text=True))
        return next(c for c in clients if c['pid']==workers[role]['pid'])

    def focus(role):
        subprocess.run(['hyprctl','dispatch','focuswindow','address:'+client(role)['address']],check=True,stdout=subprocess.DEVNULL)
        time.sleep(.1)

    def keys(*values):
        focus(primary)
        subprocess.run(['xdotool','key','--clearmodifiers',*values],check=True)
        time.sleep(.15)

    def config(role):
        c=configparser.ConfigParser(interpolation=None)
        c.read(Path(workers[role]['data_root'])/'PCSX2x6/inis/PCSX2.ini')
        return (c.getint('Lightgun','SindenBorder',fallback=1),c.getint('Lightgun','BorderWidth',fallback=2))

    def layouts():
        output={}
        for role,reader in readers.items():
            base=reader.fields_address-symbols['g_FrameCount']
            values=struct.unpack('<9f',reader.read(base+symbols['tc3::sinden::current'],36))
            actual=struct.unpack('<4f',reader.read(base+symbols['s_last_draw_rect'],16))
            assert all(abs(a-b)<.01 for a,b in zip(values[4:8],actual)), 'Aim rect differs from presented image'
            output[role]={'outer':values[:4],'content':values[4:8],'white':values[8]}
        return output

    def capture(role,name,geometry):
        focus(role)
        c=client(role);x,y=c['at'];w,h=c['size']
        path=args.evidence/(name+'.png')
        subprocess.run(['grim','-g',f'{x},{y} {w}x{h}',str(path)],check=True)
        r=geometry['outer'];t=geometry['white']
        with Image.open(path) as image:
            # Check the centers of all four independently drawn border edges.
            for px,py in (((r[0]+r[2])/2,r[1]+t/2),((r[0]+r[2])/2,r[3]-t/2),
                          (r[0]+t/2,(r[1]+r[3])/2),(r[2]-t/2,(r[1]+r[3])/2)):
                color=image.convert('RGB').getpixel((int(px),int(py)))
                assert min(color)>220 and max(color)-min(color)<3, f'{role}: missing neutral bright border at {px},{py}: {color}'
        return str(path)

    original={r:config(r) for r in workers}
    assert len(set(original.values()))==1, 'Start from matching settings'
    mode,width=original[primary]
    open_menu=False
    test_toggled=False
    report={}
    try:
        keys('F10','Tab','Home','Down','Down');open_menu=True
        while mode!=2:
            keys('Right');mode=(mode+1)%3
        wait_for(lambda:all(config(r)[0]==2 for r in workers),'forced border persisted')
        time.sleep(1.2)
        first=layouts()
        assert all(v['white']>0 for v in first.values()),'Missing per-view border'
        fields={r:v.fields() for r,v in readers.items()}
        time.sleep(.3)
        assert fields=={r:v.fields() for r,v in readers.items()},'Border edit advanced guest'
        # Capture unoccluded cabinets. Hide the left menu only by focusing the
        # right; the primary menu remains paused, so the other view is static.
        for role in workers:
            if role!=primary: capture(role,'border-'+role,first[role])
        keys('Down','Right' if width<5 else 'Left')
        changed=width+(1 if width<5 else -1)
        wait_for(lambda:all(config(r)[1]==changed for r in workers),'border width persisted')
        time.sleep(.3)
        second=layouts()
        assert all(second[r]['white']!=first[r]['white'] for r in workers),'Width did not update live'
        keys('Left' if width<5 else 'Right','Up')
        while mode!=original[primary][0]:
            keys('Right');mode=(mode+1)%3
        wait_for(lambda:all(config(r)==original[r] for r in workers),'border preferences restored')
        report.update(border_layout=first,aim_rect_matches=True,paused_fields_unchanged=True,settings_restored=True)
        # Display -> Performance -> Rendering -> Cabinet. Toggle the original
        # test DIP and verify guest execution resumes, then toggle it back.
        keys('Tab','Tab','Tab','Home','Return');open_menu=False;test_toggled=True
        logfile=Path(workers[primary]['log'])
        wait_for(lambda:'[Cabinet maintenance]' in logfile.read_text(),'original test switch action')
        wait_for(lambda:readers[primary].fields()>fields[primary]+6,'maintenance resumed original guest')
        time.sleep(.4)
        keys('F10','Home','Return');test_toggled=False
        wait_for(lambda:len(re.findall(r'\[Cabinet maintenance\]',logfile.read_text()))>=2,'test switch restored')
        report['original_test_toggle_and_resume']=True
        (args.evidence/'validation.json').write_text(json.dumps(report,indent=2)+'\n')
        print('PASS per-view live borders, unchanged paused guest, original aim mapping, persistence/restoration and cabinet test toggle/resume')
    finally:
        if test_toggled:
            if not open_menu: keys('F10')
            keys('Home','Return');open_menu=False
        if open_menu:
            # Still on Display: restore width and mode before resuming.
            keys('Home','Down','Down','Down')
            current=config(primary)[1]
            while current!=width:
                step=1 if current<width else -1;keys('Right' if step>0 else 'Left');current+=step
            keys('Up');current=config(primary)[0]
            while current!=original[primary][0]:keys('Right');current=(current+1)%3
            keys('Escape')
        for reader in readers.values():reader.close()


if __name__=='__main__':main()
