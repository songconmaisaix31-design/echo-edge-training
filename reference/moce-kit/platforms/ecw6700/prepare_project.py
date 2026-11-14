"""Create a new SDK app from the contestant starter. Never overwrite a project."""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--sdk',required=True,type=Path,help='Prepared B02P12 code directory')
p.add_argument('--name',default='moce_contestant')
a=p.parse_args(); root=Path(__file__).resolve().parent; sdk=a.sdk.resolve()
if not a.name.replace('_','').isalnum():raise SystemExit('Invalid app name')
subprocess.run([sys.executable,'-B',str(root/'sdk-extension/tools/check_sdk.py'),str(sdk),'--adc','--gatt','--board-build'],check=True)
assets=json.loads((root/'reference-assets.json').read_text(encoding='utf-8'))
for rel,expected in assets.items():
    if hashlib.sha256((sdk/rel).read_bytes()).hexdigest()!=expected:raise SystemExit('SDK asset mismatch: '+rel)
app=sdk/'Boards/ecw6700f'/a.name
if app.exists():raise SystemExit('App exists; choose a new name')
shutil.copytree(root/'starter',app)
shutil.copytree(root/'bsp',app/'integration/bsp')
shutil.copytree(root/'board',app/'integration/board')
for rel in assets:
    dst=app/'ref_bin'/Path(rel).name;dst.parent.mkdir(exist_ok=True)
    shutil.copyfile(sdk/rel,dst)
f=app/'Make.defs';f.write_text(f.read_text(encoding='utf-8').replace('APP_PLACEHOLDER',a.name),encoding='utf-8')
print(app)
