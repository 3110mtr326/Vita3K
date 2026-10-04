"""Guard against silently omitting new top-level GxmContextState fields.

This is a source-schema coverage check, not a C++ parser or a runtime test.
Nested surface/texture fields and bit packing are covered by the codec tests.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
types = (root / 'vita3k/renderer/include/renderer/gxm_types.h').read_text()
codec = (root / 'vita3k/renderer/include/renderer/gxm_state_codec.h').read_text()
body = types.split('struct GxmContextState {', 1)[1].split('\n};', 1)[0]
body = re.sub(r'//[^\n]*', '', body)
declarations = [part.strip() for part in body.split(';') if part.strip()]
matches = re.findall(r'\b([A-Za-z_]\w*)\s*(?:=[^;]*|\{[^;]*\})?;', body)
if not matches or len(matches) != len(declarations):
    raise SystemExit('GxmContextState declarations changed: update this coverage checker')
declared = set(matches)
fields = codec.split('void context_fields(', 1)[1].split('\n}', 1)[0]
visited = set(re.findall(r'\bs\.([A-Za-z_]\w*)', fields))
if declared != visited:
    raise SystemExit(f'Schema mismatch: missing={sorted(declared-visited)}, unknown={sorted(visited-declared)}')
print(f'PASS: all {len(declared)} top-level GxmContextState fields have schema entries')
