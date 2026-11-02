# MM review (document): follow-ups of fuzz_mm.py's findings, rendered with pqdaw --render.
#  loop_voice   PA (notes) -> WT -> Env -> Saturator with one wire Saturator.out -> PA.hz (a loop back to the voice head)
#  loop_voice2  the same plus a second, unrelated MIDI track (does it go silent too?)
#  amp_<k>      base_full with the Wavetable's cycle scaled by k (finite floats, accepted by the reader)
# Usage: python -I extra_cases.py <repo root> <gen dir> <out dir>
import base64, copy, json, math, os, struct, subprocess, sys

root, gen, out = sys.argv[1], sys.argv[2], sys.argv[3]
exe = os.path.join(root, 'build', 'check', 'src', 'app', 'pqdaw.exe')
os.makedirs(out, exist_ok=True)


def save(name, d):
    p = os.path.join(out, name)
    os.makedirs(p, exist_ok=True)
    json.dump(d, open(os.path.join(p, 'project.pqd'), 'w', encoding='utf-8'), indent=1)
    return p


def render(p):
    wav = os.path.join(p, 'out.wav')
    if os.path.exists(wav):
        os.remove(wav)
    r = subprocess.run([exe, '--project', p, '--render', wav, '--seconds', '1.5'], capture_output=True, timeout=120)
    msg = (r.stderr or r.stdout).decode('utf-8', 'replace').strip().splitlines()
    st = ''
    if r.returncode == 0:
        b = open(wav, 'rb').read()
        i = b.find(b'data')
        n = struct.unpack('<I', b[i + 4:i + 8])[0] // 4
        v = struct.unpack(f'<{n}f', b[i + 8:i + 8 + 4 * n])
        bad = sum(1 for x in v if not math.isfinite(x))
        st = f'nonfinite={bad}/{n} peak={max((abs(x) for x in v if math.isfinite(x)), default=0):.3g}'
    return r.returncode, (msg[-1] if msg else ''), st


d = json.load(open(os.path.join(gen, 'grp_head_pa_wt_a', 'project.pqd'), encoding='utf-8'))
devs = d['tracks'][0]['devices']
pa, sat = devs[0]['id'], devs[-1]['id']
d1 = copy.deepcopy(d)
d1.setdefault('wires', []).append({'id': 900, 'from': sat, 'port': 'out', 'to': pa, 'param': 'hz'})
print('loop_voice', render(save('loop_voice', d1)))
d2 = copy.deepcopy(d1)
t2 = copy.deepcopy(json.load(open(os.path.join(gen, 'grp_interleaved_a', 'project.pqd'), encoding='utf-8'))['tracks'][0])
t2['id'] = 800
for i, x in enumerate(t2['devices']):
    x['id'] = 801 + i
t2['clips'] = [dict(c, id=850 + k) for k, c in enumerate(t2['clips'])]
for k, c in enumerate(t2['clips']):
    for j, n in enumerate(c.get('notes', [])):
        n['id'] = 860 + 10 * k + j
d2['tracks'].append(t2)
print('loop_voice2 (+ an unrelated track)', render(save('loop_voice2', d2)))
print('no loop, same two tracks', render(save('noloop2', dict(d2, wires=[w for w in d2['wires'] if w['id'] != 900]))))

b = json.load(open(os.path.join(gen, 'base_full', 'project.pqd'), encoding='utf-8'))
for amp in [1e3, 1e6, 1e10, 1e15, 1e19, 1e25, 1e30, 3e38 / 1.0]:
    d3 = copy.deepcopy(b)
    wt = d3['tracks'][0]['devices'][1]
    cyc = wt['meta']['']['data']['keyframes'][0]['cycle']
    vals = struct.unpack('<2048f', base64.b64decode(cyc['$f32']))
    mx = max(abs(x) for x in vals)
    cyc['$f32'] = base64.b64encode(struct.pack('<2048f', *[x / mx * amp for x in vals])).decode()
    print(f'amp {amp:g}', render(save(f'amp_{amp:g}', d3)))
