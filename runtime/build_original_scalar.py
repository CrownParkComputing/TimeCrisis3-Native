#!/usr/bin/env python3
"""Compile the unchanged pinned scalar opcode translation unit for a small oracle.

Function/data sections let the test link only the original scalar handlers;
no replacement opcode implementations or full-host dummy services are linked.
"""
import argparse, json, shlex, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core-build',type=Path,required=True)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
original=(a.source/'pcsx2/R5900OpcodeImpl.cpp').resolve()
entries=json.loads((a.core_build/'compile_commands.json').read_text())
e=next(e for e in entries if Path(e['file']).resolve()==original)
command=e.get('arguments') or shlex.split(e['command'])
command[command.index('-o')+1]=str(a.output.resolve())
command += ['-ffunction-sections','-fdata-sections']
a.output.parent.mkdir(parents=True,exist_ok=True)
subprocess.run(command,cwd=e['directory'],check=True)
