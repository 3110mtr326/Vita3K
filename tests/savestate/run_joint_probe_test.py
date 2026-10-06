"""Use VITA3K_TEST_INCLUDES for cached dependency include directories."""
from pathlib import Path
import argparse,os,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--compiler',required=True);a=p.parse_args()
s=Path(__file__).resolve().parents[2]
includes=[]
for path in os.environ.get('VITA3K_TEST_INCLUDES','').split(os.pathsep):
    if path: includes+=['-I',path]
with tempfile.TemporaryDirectory() as d:
    exe=Path(d)/'joint.exe'
    subprocess.run([a.compiler,'-std=c++20','-O2','-DFMT_HEADER_ONLY','-Wall','-Wextra','-Werror',*includes,
        str(s/'tests/savestate/joint_probe_test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
