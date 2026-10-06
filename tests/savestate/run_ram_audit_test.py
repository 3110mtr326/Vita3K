from pathlib import Path
import argparse, subprocess, tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);a=p.parse_args()
s=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as d:
    exe=Path(d)/'ram.exe'
    subprocess.run([a.compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(s/'vita3k/app/include'),str(s/'tests/savestate/ram_audit_test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
