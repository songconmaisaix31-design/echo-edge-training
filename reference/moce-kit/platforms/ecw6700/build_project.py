"""Build an existing ECW SDK app; never flashes. Native Windows tool layout or Linux PATH."""
from pathlib import Path
import argparse, os, shutil, subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path)
p.add_argument('--name',default='moce_contestant')
p.add_argument('--windows-tools',type=Path,help='Root containing compiler/, make/, git/')
p.add_argument('--jobs',type=int,default=4)
a=p.parse_args();sdk=a.sdk.resolve();app=sdk/'Boards/ecw6700f'/a.name
if not a.name.replace('_','').isalnum() or not (app/'Make.defs').is_file():raise SystemExit('Invalid app')
if a.jobs<1:raise SystemExit('jobs must be positive')
env=dict(os.environ)
if a.windows_tools:
    t=a.windows_tools.resolve()
    env['PATH']=os.pathsep.join(str(t/x) for x in ['compiler/bin','make/bin','git/usr/bin','git/mingw64/bin'])+os.pathsep+env['PATH']
    command=[str(t/'make/bin/make.exe')]
    extra=['SHELL='+(t/'git/usr/bin/sh.exe').as_posix(),'HOSTEXEEXT=.exe','OS_TYPE_CFG=Windows','RISCV_GCC_PATH='+(t/'compiler/bin').as_posix()+'/']
else:
    if os.name=='nt':raise SystemExit('Windows requires --windows-tools')
    gcc=shutil.which('riscv64-unknown-elf-gcc')
    if not gcc:raise SystemExit('Put the fixed ECW compiler on PATH')
    command=['make'];extra=['HOSTEXEEXT=','OS_TYPE_CFG=Linux','RISCV_GCC_PATH='+Path(gcc).parent.as_posix()+'/']
command+=['-C',sdk.as_posix(),'-j'+str(a.jobs),'all','BOARD_DIR='+app.as_posix(),'TOPDIR='+sdk.as_posix(),'APP_NAME='+a.name,'PRODUCT_NAME=ecw6700f','PARALLEL=-j'+str(a.jobs)]+extra
raise SystemExit(subprocess.run(command,env=env).returncode)
