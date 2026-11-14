"""Check this BSP's reviewed SDK inputs. Does not execute or modify the SDK."""
from pathlib import Path
import argparse
import hashlib
import json
p = argparse.ArgumentParser()
p.add_argument("sdk_code", type=Path)
p.add_argument("--baseline", choices=("B02P03", "B02P12"), default="B02P12",
               help="Explicit reviewed SDK identity; current projects default to B02P12; B02P03 is compatibility fallback")
p.add_argument("--gatt", action="store_true", help="Require explicit GATT database extension inputs")
p.add_argument("--board-build", action="store_true", help="Require opt-in project object isolation build rules")
p.add_argument("--adc", action="store_true", help="Require checked ADC extension")
a = p.parse_args()
manifest = json.loads(Path(__file__).with_name(a.baseline.lower() + "-sdk-identity.json").read_text(encoding="utf-8"))
files = dict(manifest["files"])
if a.gatt:
    files.update(manifest["gatt_files"])
if a.board_build:
    files.update(manifest["board_build_files"])
if a.adc:
    files.update(manifest["adc_files"])
errors = []
for relative, expected in files.items():
    path = a.sdk_code / relative
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        errors.append(relative)
print(json.dumps({"baseline": manifest["baseline"], "checked": len(files), "mismatches": errors}))
raise SystemExit(bool(errors))
