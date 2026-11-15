import getpass
import os
from pathlib import Path
import subprocess
import sys


environment = os.environ.copy()
environment["ECHO_VERIFICATION_PASSWORD"] = getpass.getpass("工作站登录密码：")
subprocess.run(["node", str(Path(__file__).with_suffix(".cjs")), *sys.argv[1:]],
               env=environment, check=True)
