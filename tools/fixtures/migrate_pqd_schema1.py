"""Rewrites schema-1 project files (PPQ 27,720) as schema 2 (PPQ 221,760) in place, keeping their formatting.

    python tools/fixtures/migrate_pqd_schema1.py path/to/project.pqd [more.pqd ...]

The same transform as the loader's built-in migration (src/document/src/serialize.cpp, T0.5-fix1): every tick
member ("tick", "start", "end", "length", "contentStart") is multiplied by 8 and "schemaVersion" becomes 2. It
works on the text so hand-written fixtures keep their layout, line endings, BOM and key order; in a .pqd those
member names only ever hold ticks. Each file is checked afterwards by parsing both versions and comparing them
value by value. Files that are not schema 1 are left alone. For fixtures written by a generator (a test with
PQ_UPDATE_FIXTURES=1, or tools/fixtures/make_*.py) regenerate them instead.
"""

import json
import re
import sys

TICK_KEYS = ("tick", "start", "end", "length", "contentStart")
_TICK = re.compile(r'"(' + "|".join(TICK_KEYS) + r')"(\s*):(\s*)(-?\d+)(?![\d.eE])')
_VERSION = re.compile(r'"schemaVersion"(\s*):(\s*)1(?![\d.eE])')


def _check(old, new, key=None, path="$"):
    if isinstance(old, dict):
        assert isinstance(new, dict) and old.keys() == new.keys(), path
        for k in old:
            _check(old[k], new[k], k, path + "." + k)
    elif isinstance(old, list):
        assert isinstance(new, list) and len(old) == len(new), path
        for i, (a, b) in enumerate(zip(old, new)):
            _check(a, b, key, "%s[%d]" % (path, i))
    elif path == "$.schemaVersion":
        assert old == 1 and new == 2, path
    elif key in TICK_KEYS and isinstance(old, int) and not isinstance(old, bool):
        assert new == 8 * old, path
    else:
        assert old == new, path


def migrate(path):
    with open(path, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw[3:].decode("utf-8") if bom else raw.decode("utf-8")
    old = json.loads(text)
    if old.get("schemaVersion") != 1:
        print("skipped (schema %s): %s" % (old.get("schemaVersion"), path))
        return
    new_text = _TICK.sub(lambda m: '"%s"%s:%s%d' % (m.group(1), m.group(2), m.group(3), 8 * int(m.group(4))), text)
    new_text, n = _VERSION.subn(r'"schemaVersion"\1:\g<2>2', new_text, count=1)
    assert n == 1, path
    _check(old, json.loads(new_text))
    with open(path, "wb") as f:
        f.write((b"\xef\xbb\xbf" if bom else b"") + new_text.encode("utf-8"))
    print("migrated: " + path)


if __name__ == "__main__":
    for p in sys.argv[1:]:
        migrate(p)
