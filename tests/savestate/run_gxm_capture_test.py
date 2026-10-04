# Vita3K emulator project
# Copyright (C) 2026 Vita3K team
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-only regression: python run_gxm_capture_test.py --compiler g++.

Uses production capture function and helper with model context/backend storage.
It does not link the emulator, run Android, or validate real GPU state.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    module = (root / 'vita3k/modules/SceGxm/SceGxm.cpp').read_text(encoding='utf-8')
    start = module.index('ContextCaptureResult capture_context_records(')
    end = module.index('\nstd::vector<std::pair<uint32_t, uint32_t>> get_host_object_ranges', start)
    template = Path(__file__).with_name('gxm_context_capture_test.cpp').read_text(encoding='utf-8')
    marker = '// PROVIDER_BODY: test driver inserts the unmodified production function here.'
    assert template.count(marker) == 1
    source = template.replace(marker, 'namespace gxm {\n' + module[start:end] + '\n}')
    save_source = (root / 'vita3k/app/src/savestate.cpp').read_text(encoding='utf-8')
    load_start = save_source.index('    const auto saved_graphics = gxm::read_context_records(in);')
    load_end = save_source.index('    // -- Memory --', load_start)
    source = source.replace('// LOAD_GRAPHICS_BODY', save_source[load_start:load_end])
    with tempfile.TemporaryDirectory(prefix='gxm-capture-') as temporary:
        temporary = Path(temporary)
        test = temporary / 'test.cpp'
        test.write_text(source, encoding='utf-8')
        executable = temporary / 'test.exe'
        command = [args.compiler, '-std=c++23', '-O2', '-pthread', '-Wall', '-Wextra',
                   '-Werror', '-DGXM_PROVIDER_TEST']
        for directory in [root / 'vita3k', *sorted((root / 'vita3k').glob('*/include'))]:
            command += ['-I', str(directory)]
        subprocess.run(command + [str(test), '-o', str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == '__main__':
    main()
