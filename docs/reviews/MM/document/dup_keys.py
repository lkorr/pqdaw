# MM review (document): duplicate keys in the new members (the reader must refuse them, naming the path).
# Usage: python -I dup_keys.py <repo root> <gen dir> <out dir>
import os, subprocess, sys

root, gen, out = sys.argv[1], sys.argv[2], sys.argv[3]
exe = os.path.join(root, 'build', 'check', 'src', 'app', 'pqdaw.exe')
base = open(os.path.join(gen, 'base_full', 'project.pqd'), encoding='utf-8').read()
cases = {
    'curves': ('"curves": {"freq1": "linear"}', '"curves": {"freq1": "linear", "freq1": "octave"}'),
    'free': ('"free": true, "group": 42', '"free": true, "free": false, "group": 42'),
    'group': ('"group": 42', '"group": 42, "group": 11'),
    'chain': ('"chain": true', '"chain": true, "chain": false'),
    'law': ('"law": "travel"', '"law": "travel", "law": "add"'),
    'chainInput': ('"chainInput": "-"', '"chainInput": "-", "chainInput": "pm"'),
    'meta path': ('"meta": {"": {"type": "vca", "version": 1}}', '"meta": {"": {"type": "vca", "version": 1}, "": {"type": "vca", "version": 1}}'),
    'scale': ('"scale": 1.75', '"scale": 1.75, "scale": 2'),
}
for name, (a, b) in cases.items():
    if a not in base:
        print(name, 'ANCHOR MISSING')
        continue
    d = os.path.join(out, 'dup_' + name.replace(' ', '_'))
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'project.pqd'), 'w', encoding='utf-8').write(base.replace(a, b, 1))
    r = subprocess.run([exe, '--project', d, '--render', os.path.join(d, 'o.wav'), '--seconds', '0.2'], capture_output=True)
    print(name, r.returncode, (r.stderr or r.stdout).decode('utf-8', 'replace').strip().splitlines()[-1][:200])
