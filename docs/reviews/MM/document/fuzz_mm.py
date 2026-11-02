# MM review (document): (1) the wire-law DC probes and (2) the group round-trip renders that probe_mm.exe generated,
# (3) migration variants of the law probes (schema 15 → Units, schema 12 → Travel), and (4) a mutation fuzzer over
# base_full (a schema-16 project with every MM field: a chain Wavetable / Envelope / VCA voice run, a user device with a
# nested meta_phase_acc, a group with chain links, free devices, note / cross-track / travel wires, knob curves,
# Oscilloscope / Spectrogram instances, scaled nudges). Every case is loaded and rendered with
# `pqdaw --project <dir> --render out.wav --seconds S`, counting crashes (exit code not 0/1), hangs (timeout), NaN/Inf
# samples in accepted renders, accepted cases that are invalid by the spec, and refusals whose message names no path.
# Usage: python -I fuzz_mm.py <repo root> <gen dir (probe_mm output)> <out dir> [random cases] [jobs]
import concurrent.futures as cf
import copy, json, math, os, random, re, shutil, struct, subprocess, sys

root, gen, out = sys.argv[1], sys.argv[2], sys.argv[3]
nrand = int(sys.argv[4]) if len(sys.argv) > 4 else 300
jobs = int(sys.argv[5]) if len(sys.argv) > 5 else 3
exe = os.path.join(root, 'build', 'check', 'src', 'app', 'pqdaw.exe')
os.makedirs(out, exist_ok=True)
log = open(os.path.join(out, 'fuzz_log.txt'), 'w', encoding='utf-8')


def wav_read(path):
    try:
        b = open(path, 'rb').read()
    except OSError:
        return None
    i = 12
    fmt = data = None
    while i + 8 <= len(b):
        cid = b[i:i + 4]
        sz = struct.unpack('<I', b[i + 4:i + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', b[i + 8:i + 24])
        if cid == b'data':
            data = b[i + 8:i + 8 + sz]
        i += 8 + sz + (sz & 1)
    if fmt is None or data is None:
        return None
    n = len(data) // 4
    return fmt[1], data, struct.unpack(f'<{n}f', data[:n * 4])


def render(pdir, wav, seconds, timeout=90):
    if os.path.exists(wav):
        os.remove(wav)
    try:
        r = subprocess.run([exe, '--project', pdir, '--render', wav, '--seconds', str(seconds)], capture_output=True,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'hang', ''
    msg = (r.stderr or r.stdout).decode('utf-8', 'replace').strip().splitlines()
    return r.returncode, (msg[-1] if msg else '')


# ---- (1) wire laws ------------------------------------------------------------------------------------------------
expect = {}
for line in open(os.path.join(gen, 'probe_stdout.txt'), encoding='utf-8'):
    m = re.match(r'law (\S+) expect (\S+)', line)
    if m:
        expect[m.group(1)] = float(m.group(2))


def last_sample(pdir, tag):
    wav = os.path.join(out, f'law_{tag}.wav')
    code, msg = render(pdir, wav, 0.25)
    if code != 0:
        return None, f'code={code} {msg}'
    w = wav_read(wav)
    return w[2][-2], ''  # the last frame's left sample


def law_cases():
    res = []
    for name, ex in expect.items():
        res.append((name, os.path.join(gen, 'law_' + name), os.path.join(gen, 'law_' + name + '_ref'), ex))
    return res


log.write('== wire laws (wired last sample / reference knob-at-expected last sample; both scale the same way)\n')
lawbad = 0
# The scale from param value to output sample (track + master): measured on linear_declared_ref (knob 1100).
scale_v, _ = last_sample(os.path.join(gen, 'law_linear_declared_ref'), 'scale')
scale = scale_v / 1100.0
log.write(f'scale (out sample per unit) = {scale!r}\n')
for name, wired, ref, ex in law_cases():
    a, ea = last_sample(wired, name)
    b, eb = last_sample(ref, name + '_ref')
    if a is None or b is None:
        log.write(f'law {name}: ERROR {ea} {eb}\n')
        lawbad += 1
        continue
    va, vb = a / scale, b / scale
    good = abs(va - ex) <= 1e-4 * max(1.0, abs(ex))
    lawbad += 0 if good else 1
    log.write(f'law {name}: wired={va:.7g} ref(knob at expect)={vb:.7g} expect={ex:.7g} {"OK" if good else "MISMATCH"}\n')
log.flush()

# ---- (3) migration variants of the law probes -------------------------------------------------------------------
log.write('== migration: the wired law projects re-labelled as older schemas\n')


def migrate_case(name, fn, ex, tag):
    d = json.load(open(os.path.join(gen, 'law_' + name, 'project.pqd'), encoding='utf-8'))
    fn(d)
    cdir = os.path.join(out, 'mig_' + tag)
    shutil.rmtree(cdir, ignore_errors=True)
    os.makedirs(cdir)
    open(os.path.join(cdir, 'project.pqd'), 'w', encoding='utf-8').write(json.dumps(d, indent=1))
    v, e = last_sample(cdir, 'mig_' + tag)
    if v is None:
        log.write(f'mig {tag}: refused/err {e}\n')
        return False
    v /= scale
    good = abs(v - ex) <= 1e-4 * max(1.0, abs(ex))
    log.write(f'mig {tag}: value={v:.7g} expect={ex:.7g} {"OK" if good else "MISMATCH"}\n')
    return good


def to15(d):  # a TX.patching (schema 13-15) Add wire: no "law" and no curves → Units: knob + c in units
    d['schemaVersion'] = 15
    for w in d.get('wires', []):
        w.pop('law', None)
    for t in d['tracks']:
        for dv in t['devices']:
            dv.pop('curves', None)


def to12(amount):
    def f(d):  # a schema-12 wire: an amount, no law → Travel
        d['schemaVersion'] = 12
        for w in d.get('wires', []):
            w.pop('law', None)
            w['amount'] = amount
        for t in d['tracks']:
            for dv in t['devices']:
                dv.pop('curves', None)
    return f


migbad = 0
lo, hi = 20.0, 20000.0
tr = lambda v: math.log(v / lo) / math.log(hi / lo)
octv = lambda t: lo * (hi / lo) ** min(1.0, max(0.0, t))
migbad += 0 if migrate_case('octave_declared', to15, 1000.1, 's15_units_octave') else 1
migbad += 0 if migrate_case('linear_declared', to15, 1100.0, 's15_units_linear') else 1
migbad += 0 if migrate_case('db_linear', to15, 7.0, 's15_units_db (linear gain + 6)') else 1
migbad += 0 if migrate_case('travel_octave', to12(0.5), octv(tr(1000) + 0.1), 's12_travel_octave (export source)') else 1
migbad += 0 if migrate_case('travel_octave', to12(-1.0), octv(tr(1000) - 0.2), 's12_travel_octave amount -1') else 1
log.flush()

# ---- (2) group round trips: series / grouped / ungrouped renders bit-identical -----------------------------------
log.write('== group round trips (renders of the series a, the group b, the ungrouped c; data bytes compared)\n')
grpbad = 0
names = sorted({m.group(1) for m in (re.match(r'grp_(.+)_[abc]$', n) for n in os.listdir(gen)) if m})
for n in names:
    datas = []
    for s in 'abc':
        p = os.path.join(gen, f'grp_{n}_{s}')
        if not os.path.isdir(p):
            datas.append(None)
            continue
        wav = os.path.join(out, f'grp_{s}.wav')
        code, msg = render(p, wav, 3)
        w = wav_read(wav) if code == 0 else None
        datas.append(w)
        if code != 0:
            log.write(f'grp {n} {s}: code={code} {msg}\n')
    if any(x is None for x in datas):
        grpbad += 1
        log.write(f'grp {n}: MISSING RENDER\n')
        continue
    peak = max(abs(v) for v in datas[0][2])
    ab = max(abs(x - y) for x, y in zip(datas[0][2], datas[1][2]))
    ac = datas[0][1] == datas[2][1]
    ok = datas[0][1] == datas[1][1] and ac
    grpbad += 0 if ok else 1
    log.write(f'grp {n}: peak={peak:.3g} series==group {datas[0][1] == datas[1][1]} (max|d|={ab:.3g}) '
              f'series==ungrouped {ac}\n')
log.flush()

# ---- (4) the mutation fuzzer ------------------------------------------------------------------------------------
base = json.load(open(os.path.join(gen, 'base_full', 'project.pqd'), encoding='utf-8'))
keys = base['tracks'][0]
ID = {'pa': 6, 'wt': 7, 'env': 8, 'vca': 9, 'user': 10, 'grp': 42, 'sat': 11, 'lfo': 12, 'util': 13, 'eq': 14,
      'scope': 15, 'spg': 16, 'lfo2': 17, 'pafree': 20, 'binst': 18, 'bflt': 19, 'avca': 21}
TRK = {'keys': 3, 'bass': 4, 'aud': 5}


def dev(d, name):
    for t in d['tracks']:
        for x in t['devices']:
            if x['id'] == ID[name]:
                return x
    raise KeyError(name)


def track_of(d, name):
    for t in d['tracks']:
        for x in t['devices']:
            if x['id'] == ID[name]:
                return t
    raise KeyError(name)


def wire(d, wid):
    for w in d['wires']:
        if w['id'] == wid:
            return w
    raise KeyError(wid)


cases = []  # (name, mutate(d) or text mutator, expect: 'reject' | 'accept' | '?')


def add(name, f, exp='?', raw=None):
    cases.append((name, f, exp, raw))


def setdev(name, key, val, exp='?'):
    def f(d):
        dev(d, name)[key] = val
    add(f'{name}.{key}={str(val)[:40]}', f, exp)


def setmeta(name, key, val, exp='?', path=''):
    def f(d):
        dev(d, name)['meta'][path][key] = val
    add(f'{name}.meta[{path!r}].{key}={str(val)[:40]}', f, exp)


NUMS = [float('nan'), float('inf'), -float('inf'), 1e308, -1e308, 5e-324, -0.0, 2**63, -(2**63), 2**64]
WEIRD = [None, 'x', [], {}, True]

# Meta instances: type / version.
for v in WEIRD + [1, 'nope', 'Phase_Acc', 'phase_acc ', '']:
    setmeta('pa', 'type', v, 'reject')
for v in [0, -1, 2, 2**32 + 1, 1e308, '1', 1.5, None]:
    setmeta('wt', 'version', v, 'reject')
# A type changed under existing data: the wavetable's keyframes under other types; another type's state on a stock name.
for t in ['phase_acc', 'vca', 'oscilloscope', 'spectrogram', 'envelope', 'spectrum']:
    setmeta('wt', 'type', t, 'reject')
for t in ['wavetable', 'vca']:
    setmeta('scope', 'type', t, 'reject')  # data {} under the wavetable: no keyframes
def wt_patch_vca(d):  # patch says vca, state says wavetable
    dev(d, 'wt')['patch'] = 'meta:vca'
add('wt patch meta:vca, state wavetable', wt_patch_vca, 'reject')
def pa_patch_unknown(d):
    dev(d, 'pa')['patch'] = 'meta:nope'
add('pa patch meta:nope', pa_patch_unknown, 'reject')
def meta_on_stock(d):
    dev(d, 'sat')['meta'] = {'': {'type': 'vca', 'version': 1}}
add('meta on a stock saturator', meta_on_stock, 'reject')
def meta_on_group(d):
    dev(d, 'grp')['meta'] = {'': {'type': 'vca', 'version': 1}}
add('meta on the group device', meta_on_group, 'reject')
def meta_empty(d):
    dev(d, 'vca')['meta'] = {}
add('vca meta {}', meta_empty, 'reject')
def meta_missing(d):
    dev(d, 'vca').pop('meta')
add('vca meta missing', meta_missing, '?')
for p in ['osc1', 'a..b', ' ', 'a b', 'x/y', 'é', 'a' * 3000, '0', '-']:
    def f(d, p=p):
        dev(d, 'vca')['meta'][p] = {'type': 'vca', 'version': 1}
    add(f'vca meta path {p[:20]!r} (chain device, nested path)', f, 'reject')
for p in ['m', 'nothere', 'm.x']:
    def f(d, p=p):
        dev(d, 'user')['meta'] = {p: {'type': 'phase_acc', 'version': 1, 'params': {'freq': 3.0}}}
    add(f'user device nested meta {p!r}', f, 'accept' if p == 'm' else 'reject')
def nested_wrong_type(d):
    dev(d, 'user')['meta'] = {'m': {'type': 'wavetable', 'version': 1}}
add('user device nested meta m typed wavetable (call is meta_phase_acc)', nested_wrong_type, 'reject')
for v in NUMS + [1e9, -1e9]:
    def f(d, v=v):
        dev(d, 'user')['meta'] = {'m': {'type': 'phase_acc', 'version': 1, 'params': {'freq': v}}}
    add(f'nested pa freq={v}', f, 'reject' if not math.isfinite(v) else '?')
# Chain meta params / settings (the device's own).
for name, par in [('pa', 'freq'), ('pa', 'pitch'), ('wt', 'position'), ('wt', 'warp'), ('env', 'attack'),
                  ('env', 'release'), ('vca', 'value'), ('vca', 'min'), ('vca', 'max'), ('vca', 'zone'),
                  ('vca', 'bend'), ('pafree', 'freq')]:
    for v in NUMS + [1e9, -1e9, 0.0]:
        def f(d, name=name, par=par, v=v):
            dev(d, name).setdefault('params', {})[par] = v
        add(f'{name}.params.{par}={v}', f, 'reject' if not math.isfinite(v) else '?')
for name, s in [('wt', 'mode'), ('wt', 'warpMode'), ('wt', 'os'), ('vca', 'curve')]:
    for v in [-1, 2, 5, 1.5, 1e308, 2**40, float('nan')]:
        def f(d, name=name, s=s, v=v):
            dev(d, name).setdefault('settings', {})[s] = v
        add(f'{name}.settings.{s}={v}', f, '?')
for name in ['pa', 'vca', 'scope']:
    def f(d, name=name):
        dev(d, name).setdefault('params', {})['bogus'] = 1.0
    add(f'{name}.params.bogus', f, '?')
def vca_min_gt_max(d):
    dev(d, 'vca')['params'] = {'min': 5.0, 'max': -5.0, 'value': 0.5}
add('vca min > max', vca_min_gt_max, 'accept')
def vca_bend(d):
    dev(d, 'vca')['params'] = {'min': -40000.0, 'max': 40000.0, 'value': 1.0, 'bend': 1.0, 'zone': 1e-9}
    dev(d, 'vca')['settings'] = {'curve': 1}
add('vca bend extremes', vca_bend, '?')

# Wavetable data.
cyc0 = dev(base, 'wt')['meta']['']['data']['keyframes'][0]['cycle']['$f32']
import base64
def f32(vals):
    return {'$f32': base64.b64encode(struct.pack(f'<{len(vals)}f', *vals)).decode()}
def keyframes(n, cycle=None, pos=None):
    def f(d):
        c = cycle if cycle is not None else {'$f32': cyc0}
        dev(d, 'wt')['meta']['']['data']['keyframes'] = [
            {'pos': (pos(i) if pos else (i / max(1, n - 1))), 'cycle': c} for i in range(n)]
    return f
add('keyframes 0', keyframes(0), 'reject')
add('keyframes 1', keyframes(1), 'accept')
add('keyframes 256', keyframes(256), 'accept')
add('keyframes 257', keyframes(257), 'reject')
add('keyframes 100000 (4-sample cycles)', keyframes(100000, f32([0, 1, 0, -1])), 'reject')
for n in [0, 1, 2047, 2049, 4096, 204800]:
    add(f'cycle length {n}', keyframes(1, f32([0.25] * n)), 'reject')
add('cycle NaN samples', keyframes(1, {'$f32': base64.b64encode(struct.pack('<f', float('nan')) * 2048).decode()}), 'reject')
add('cycle Inf samples', keyframes(1, {'$f32': base64.b64encode(struct.pack('<f', float('inf')) * 2048).decode()}), 'reject')
add('cycle 3e38 samples', keyframes(1, f32([3e38] * 2048)), '?')
add('cycle denormal samples', keyframes(1, f32([1e-41] * 2048)), 'accept')
add('cycle DC 1.0', keyframes(1, f32([1.0] * 2048)), 'accept')
add('cycle bad base64', keyframes(1, {'$f32': '!!!!' * 100}), 'reject')
add('cycle base64 not x4 bytes', keyframes(1, {'$f32': base64.b64encode(b'\0' * 8191).decode()}), 'reject')
add('cycle $f32 not a string', keyframes(1, {'$f32': 5}), 'reject')
add('cycle $f32 + extra key', keyframes(1, {'$f32': cyc0, 'x': 1}), 'reject')
add('cycle as plain list of 2048 numbers', keyframes(1, [0.0] * 2048), 'reject')
add('pos decreasing', keyframes(3, pos=lambda i: 1.0 - i / 2), 'reject')
add('pos all equal', keyframes(3, pos=lambda i: 0.5), 'accept')
add('pos > 1', keyframes(2, pos=lambda i: 2.0 * i), 'reject')
add('pos -1', keyframes(2, pos=lambda i: -1.0 + i), 'reject')
add('pos NaN', keyframes(2, pos=lambda i: float('nan')), 'reject')
add('pos string', keyframes(2, pos=lambda i: 'a'), 'reject')
def wtdata(v, exp):
    def f(d):
        dev(d, 'wt')['meta']['']['data'] = v
    add(f'wavetable data = {str(v)[:50]}', f, exp)
for v in [None, 1, 'x', [], {}, {'keyframes': {}}, {'keyframes': 5}]:
    wtdata(v, 'reject')
def wtfield(k, v, exp):
    def f(d):
        dev(d, 'wt')['meta']['']['data'][k] = v
    add(f'wavetable data.{k} = {str(v)[:40]}', f, exp)
for k, v, e in [('frames', 0, 'reject'), ('frames', 257, 'reject'), ('frames', 1.5, 'reject'), ('frames', 256, 'accept'),
                ('frames', 1, 'accept'), ('frames', 1e308, 'reject'), ('removeDc', 2, 'reject'), ('normalise', 1, 'accept'),
                ('gaps', [], 'accept'), ('gaps', ['spectral'], 'reject'), ('extra', 1, 'reject'),
                ('keyframes2', [], 'reject')]:
    wtfield(k, v, e)
def gaps_for_two(g):
    def f(d):
        keyframes(2)(d)
        dev(d, 'wt')['meta']['']['data']['gaps'] = [g]
    return f
add('two keyframes gap crossfade', gaps_for_two('crossfade'), 'accept')
add('two keyframes gap bogus', gaps_for_two('cubic'), 'reject')
def deep(n):
    def f(d):
        v = 0
        for _ in range(n):
            v = [v]
        dev(d, 'scope')['meta']['']['data'] = {'x': v}
    return f
add('scope data nested 60', deep(60), 'reject')
add('scope data nested 70', deep(70), 'reject')
add('scope data nested 250', deep(250), 'reject')
add('scope data nested 300', deep(300), 'reject')
def deep15(n):
    def f(d):
        deep(n)(d)
        d['schemaVersion'] = 15
    return f
add('scope data nested 300 in a schema-15 file (DOM migration path)', deep15(300), 'reject')
add('scope data nested 5000 in a schema-15 file', deep15(5000), 'reject')
for v in [{'mode': 1}, {'mode': 'x'}, {'timebase': 1e308}, {'a': float('nan')}, {'a': {'b': 1}}, [], 1]:
    def f(d, v=v):
        dev(d, 'scope')['meta']['']['data'] = v
    add(f'scope data = {str(v)[:40]}', f, '?')
    def g(d, v=v):
        dev(d, 'spg')['meta']['']['data'] = v
    add(f'spectrogram data = {str(v)[:40]}', g, '?')

# chainInput.
for name, v, e in [('wt', '', 'reject'), ('wt', '-', 'accept'), ('wt', 'pos', 'accept'), ('wt', 'bogus', 'reject'),
                   ('wt', 5, 'reject'), ('sat', 'x', 'reject'), ('grp', '-', 'reject'), ('pafree', 'pm', 'accept'),
                   ('pa', '-', 'accept'), ('env', 'gate', '?'), ('vca', 'in', 'accept'), ('scope', '-', 'accept')]:
    setdev(name, 'chainInput', v, e)

# free.
for name, v, e in [('pa', True, 'reject'), ('wt', True, 'accept'), ('grp', True, '?'), ('binst', True, 'reject'),
                   ('sat', False, 'reject'), ('lfo', 'yes', 'reject'), ('lfo', 1, 'reject'), ('env', True, 'accept'),
                   ('scope', True, 'accept')]:
    setdev(name, 'free', v, e)

# group membership.
for name, v, e in [('sat', 999, 'reject'), ('sat', 11, 'reject'), ('sat', 13, 'reject'), ('sat', -1, 'reject'),
                   ('sat', 0, 'reject'), ('sat', 2**32, 'reject'), ('sat', '42', 'reject'), ('sat', None, 'reject'),
                   ('grp', 42, 'reject'), ('lfo', 42, 'accept'), ('lfo2', 42, 'accept'), ('wt', 42, 'reject'),
                   ('pafree', 42, 'accept'), ('eq', 42, 'reject'), ('pa', 42, 'reject')]:
    setdev(name, 'group', v, e)
def member_other_track(d):
    s = dev(d, 'sat')
    track_of(d, 'sat')['devices'].remove(s)
    d['tracks'][1]['devices'].append(s)
add('group member in another track', member_other_track, 'reject')
def group_other_track(d):
    g = dev(d, 'grp')
    track_of(d, 'grp')['devices'].remove(g)
    d['tracks'][1]['devices'].append(g)
add('group device moved to another track (members stay)', group_other_track, 'reject')
def two_groups_cycle(d):
    t = track_of(d, 'grp')
    g2 = {'id': 900, 'patch': 'Group', 'kind': 'audioEffect', 'free': True, 'group': 42}
    t['devices'].append(g2)
    dev(d, 'grp')['group'] = 900
add('two groups members of each other', two_groups_cycle, 'reject')
def nested_group(d):
    t = track_of(d, 'grp')
    t['devices'].append({'id': 900, 'patch': 'Group', 'kind': 'audioEffect', 'free': True, 'group': 42})
add('group inside a group', nested_group, 'reject')
for k, v in [('params', {'x': 1.0}), ('settings', {'a': 1.0}), ('source', ['patch g {', '}']), ('curves', {'x': 'linear'}),
             ('kind', 'instrument'), ('chainInput', 'x'), ('name', 'G' * 10000)]:
    setdev('grp', k, v, 'accept' if k == 'name' else 'reject')
def group_instrument_slot(d):
    g = dev(d, 'grp')
    track_of(d, 'grp')['devices'].remove(g)
    d['tracks'][1]['devices'].insert(0, g)
    d['tracks'][1]['devices'].pop(1) if False else None
add('group device at the Bass track head (before BasicSynth)', group_instrument_slot, 'reject')
def group_in_rack(d):
    g = copy.deepcopy(dev(d, 'grp'))
    g['id'] = 901
    rack = {'id': 902, 'patch': 'AudioEffectRack', 'kind': 'audioEffect',
            'rack': {'chains': [{'id': 903, 'name': 'c', 'devices': [g]}]}}
    d['tracks'][2]['devices'].append(rack)
add('group device inside a rack chain', group_in_rack, 'reject')
def free_in_rack(d):
    s = copy.deepcopy(dev(d, 'lfo2'))
    s['id'] = 905
    s.pop('group', None)
    rack = {'id': 906, 'patch': 'AudioEffectRack', 'kind': 'audioEffect',
            'rack': {'chains': [{'id': 907, 'name': 'c', 'devices': [s]}]}}
    d['tracks'][2]['devices'].append(rack)
add('free device inside a rack chain', free_in_rack, 'reject')
def delete_group(d):  # members point at a deleted group
    track_of(d, 'grp')['devices'].remove(dev(d, 'grp'))
    d['wires'] = [w for w in d['wires'] if w.get('from') != 42 and w.get('to') != 42]
add('group device deleted, members and links left', delete_group, 'reject')
def delete_group_keep_wires(d):
    track_of(d, 'grp')['devices'].remove(dev(d, 'grp'))
    for x in track_of(d, 'sat')['devices']:
        x.pop('group', None)
add('group device deleted, chain links dangling', delete_group_keep_wires, 'reject')
def ungrouped_members_keep_links(d):
    for x in track_of(d, 'sat')['devices']:
        x.pop('group', None)
add('members leave the group; chain links stay', ungrouped_members_keep_links, '?')

# Knob curves.
for name, par, v, e in [('eq', 'freq1', 'cubic', 'reject'), ('eq', 'freq1', 'default', 'reject'),
                        ('eq', 'freq1', 'Octave', 'reject'), ('eq', 'freq1', 1, 'reject'), ('eq', 'freq1', None, 'reject'),
                        ('eq', 'nothere', 'linear', '?'), ('util', 'balance', 'octave', '?'), ('util', 'mute', 'linear', '?'),
                        ('util', 'chan', 'exponential', '?'), ('util', 'gain', 'octave', '?'), ('vca', 'value', 'octave', '?'),
                        ('pa', 'freq', 'linear', 'accept'), ('pa', 'freq', 'logarithmic', 'accept'),
                        ('pa', 'pitch', 'octave', '?'), ('env', 'attack', 'linear', 'accept'), ('wt', 'phase', 'exponential', '?'),
                        ('user', 'rate', 'linear', 'accept'), ('user', 'm', 'linear', '?'), ('grp', 'x', 'linear', 'reject'),
                        ('sat', '', 'linear', 'reject'), ('sat', 'a' * 5000, 'linear', '?')]:
    def f(d, name=name, par=par, v=v):
        dev(d, name).setdefault('curves', {})[par] = v
    add(f'{name}.curves.{par[:20]}={v}', f, e)
setdev('eq', 'curves', {}, '?')
setdev('eq', 'curves', [], 'reject')
setdev('eq', 'curves', 'linear', 'reject')
def curves_rack(d):
    d['tracks'][2]['devices'].append({'id': 910, 'patch': 'AudioEffectRack', 'kind': 'audioEffect',
                                      'curves': {'macro1': 'linear'}, 'rack': {'chains': []}})
add('rack with curves', curves_rack, 'reject')
add('curves duplicate key', None, 'reject', raw=('"curves": {"freq1": "linear"}', '"curves": {"freq1": "linear", "freq1": "octave"}'))
add('free duplicate key', None, 'reject', raw=('"free": true, "group": 42', '"free": true, "free": false, "group": 42'))
add('group duplicate key', None, 'reject', raw=('"group": 42', '"group": 42, "group": 11'))
add('chain duplicate key', None, 'reject', raw=('"chain": true', '"chain": true, "chain": false'))

# Wires.
for wid, k, v, e in [(36, 'law', 'mul', 'reject'), (36, 'law', 'add', 'accept'), (36, 'law', 'units', 'accept'),
                     (36, 'law', 'travel', 'reject'), (36, 'law', None, 'reject'), (36, 'law', 3, 'reject'),
                     (36, 'amount', 0.5, 'reject'), (41, 'amount', 2.0, 'reject'), (41, 'amount', float('nan'), 'reject'),
                     (41, 'amount', 1e308, 'reject'), (41, 'amount', -1.0, 'accept'), (41, 'law', 'units', 'reject'),
                     (43, 'chain', 'true', 'reject'), (43, 'chain', False, '?'), (36, 'chain', True, '?'),
                     (39, 'chain', True, '?'), (38, 'chain', True, '?'),
                     (36, 'port', 'bogus', '?'), (36, 'param', 'bogus', '?'), (36, 'param', '', 'reject'),
                     (36, 'from', 999, 'reject'), (36, 'to', 999, 'reject'), (36, 'from', -1, 'reject'),
                     (36, 'to', 0, 'reject'), (36, 'from', 2**32, 'reject'), (36, 'from', 11, '?'),
                     (36, 'to', 3, 'reject'), (38, 'notes', 5, 'reject'), (38, 'notes', 999, 'reject'),
                     (38, 'notes', 4, 'accept'), (38, 'notes', 11, 'reject'), (38, 'port', 'cents', 'reject'),
                     (38, 'port', 'gate', 'accept'), (38, 'port', 'vel', 'accept'), (38, 'from', 12, 'reject'),
                     (36, 'reducer', 'mean', 'reject'), (38, 'reducer', 'loudest', 'accept'),
                     (36, 'notes', 3, 'reject')]:
    def f(d, wid=wid, k=k, v=v):
        wire(d, wid)[k] = v
    add(f'wire {wid}.{k}={str(v)[:30]}', f, e)
def wdrop(wid, k, e):
    def f(d):
        wire(d, wid).pop(k)
    add(f'wire {wid} without {k}', f, e)
wdrop(41, 'amount', 'reject')
wdrop(38, 'notes', 'reject')
wdrop(36, 'port', 'reject')
def addwire(w, e, name):
    def f(d):
        d['wires'].append(dict(w, id=w.get('id', 990)))
        d['wires'].sort(key=lambda x: x['id'] if isinstance(x['id'], int) else 0)
    add(name, f, e)
addwire({'from': 11, 'port': 'out', 'to': 11, 'param': 'drive'}, '?', 'wire self-loop sat.out -> sat.drive')
addwire({'from': 11, 'port': 'out', 'to': 11, 'param': 'x'}, '?', 'wire self-loop sat.out -> sat.x')
addwire({'from': 13, 'port': 'out', 'to': 11, 'param': 'x'}, '?', 'wire util.out -> sat.x (loop through the group)')
addwire({'from': 14, 'port': 'out', 'to': 6, 'param': 'hz'}, '?', 'wire eq.out -> pa.hz (loop to the voice head)')
addwire({'from': 13, 'port': 'out', 'to': 11, 'param': 'x', 'chain': True}, '?', 'chain link closing a loop')
addwire({'from': 19, 'port': 'out', 'to': 11, 'param': 'x', 'chain': True}, '?', 'chain link from another track')
addwire({'from': 11, 'port': 'out', 'to': 19, 'param': 'x', 'chain': True}, '?', 'chain link into another track')
addwire({'from': 17, 'port': 'lfo', 'to': 19, 'param': 'freq', 'chain': True}, '?', 'chain link free device to other track')
addwire({'from': 42, 'port': 'input', 'to': 42, 'param': 'output'}, '?', 'group In -> its own Out')
addwire({'from': 42, 'port': 'input', 'to': 14, 'param': 'freq1'}, '?', 'group In -> outside param')
addwire({'from': 14, 'port': 'out', 'to': 42, 'param': 'output'}, '?', 'outside device -> group Out')
addwire({'from': 19, 'port': 'out', 'to': 42, 'param': 'x'}, '?', 'cross-track audio into the group x')
addwire({'from': 21, 'port': 'out', 'to': 12, 'param': 'rate'}, '?', 'cross-track wire back (aud -> keys)')
addwire({'from': 21, 'port': 'out', 'to': 21, 'param': 'value'}, '?', 'vca self-loop')
addwire({'notes': 3, 'port': 'hz', 'to': 21, 'param': 'value'}, '?', 'note wire into an audio track VCA')
addwire({'notes': 3, 'port': 'hz', 'to': 7, 'param': 'x'}, '?', 'note hz into the wavetable phase input')
addwire({'from': 36, 'port': 'lfo', 'to': 11, 'param': 'drive'}, 'reject', 'wire from a wire id')
addwire({'from': 12, 'port': 'lfo', 'to': 11, 'param': 'drive', 'id': 991}, 'reject', 'duplicate ends')
addwire({'from': 12, 'port': 'lfo', 'to': 11, 'param': 'drive', 'id': 36}, 'reject', 'duplicate wire id')
def unsorted(d):
    d['wires'].reverse()
add('wires unsorted', unsorted, 'reject')
def many_wires(n):
    def f(d):
        for i in range(n):
            d['wires'].append({'id': 10000 + i, 'from': 17, 'port': 'lfo', 'to': 14, 'param': f'p{i}'})
    return f
add('5000 wires to bogus params', many_wires(5000), '?')
add('20000 wires to bogus params', many_wires(20000), '?')
def many_real_wires(d):
    for i, (f, p) in enumerate([(12, 'lfo'), (17, 'lfo'), (20, 'out'), (10, 'lfo'), (11, 'out'), (13, 'out')]):
        for j, tp in enumerate(['freq1', 'freq2', 'freq3', 'q1', 'gain1', 'freq4']):
            d['wires'].append({'id': 20000 + i * 10 + j, 'from': f, 'port': p, 'to': 14, 'param': tp})
add('36 real wires into EQ Eight', many_real_wires, 'accept')

# View nudges / scale.
for v, e in [(0, 'reject'), (-1, 'reject'), (0.49, 'reject'), (0.5, 'accept'), (3.0, 'accept'), (3.01, 'reject'),
             (1e308, 'reject'), (float('nan'), 'reject'), (float('inf'), 'reject'), ('big', 'reject'), (None, 'reject'),
             (1, '?')]:
    def f(d, v=v):
        d['view']['patch'][1]['scale'] = v
    add(f'nudge scale={v}', f, e)
def band_scale(d):
    d['view']['patch'][0]['scale'] = 2.0
add('nudge scale on a band (track id)', band_scale, '?')
def nudge_unknown(d):
    d['view']['patch'].append({'id': 99999, 'dx': 1.0, 'dy': 1.0, 'scale': 2.0})
add('nudge for an unknown id', nudge_unknown, '?')
for v in [1e308, float('nan'), -1e308]:
    def f(d, v=v):
        d['view']['patch'][1]['dx'] = v
    add(f'nudge dx={v}', f, 'reject' if not math.isfinite(v) else '?')
def nudge_dup(d):
    d['view']['patch'].append(dict(d['view']['patch'][1]))
add('nudge duplicate id', nudge_dup, 'reject')

# Schema number lies.
for v, e in [(12, 'reject'), (13, '?'), (14, '?'), (15, '?'), (9, 'reject'), (10, 'reject'), (11, 'reject'), (17, 'reject'),
             (0, 'reject'), (-1, 'reject'), (1e308, 'reject'), ('16', 'reject'), (16.0, '?'), (None, 'reject')]:
    def f(d, v=v):
        d['schemaVersion'] = v
    add(f'schemaVersion={v} (v16 content)', f, e)
def v15_with_units(d):
    d['schemaVersion'] = 15
add('v16 Units-law content marked 15', v15_with_units, '?')
def v12_lies(d):
    d['schemaVersion'] = 12
    for w in d['wires']:
        w.pop('law', None)
        w['amount'] = 0.5
add('v16 content marked 12 (wires given amounts)', v12_lies, '?')

# Random combinations of the single mutations above that are not raw-text ones.
singles = [c for c in cases if c[1] is not None]
rng = random.Random(7)
for k in range(nrand):
    picks = rng.sample(singles, rng.randint(2, 4))
    def f(d, picks=picks):
        for _, m, _, _ in picks:
            try:
                m(d)
            except (KeyError, IndexError, TypeError, ValueError, AttributeError):
                pass
    add(f'random {k}: ' + ' | '.join(p[0][:40] for p in picks), f, '?')

PATH_RE = re.compile(r'\$[.\[]|\$:|\$\.?schemaVersion|project\.pqd')


def run_case(idx):
    name, mut, exp, raw = cases[idx]
    d = copy.deepcopy(base)
    try:
        if mut is not None:
            mut(d)
    except Exception as e:  # noqa: BLE001
        return idx, 'muterr', f'{e!r}', ''
    txt = json.dumps(d, indent=1, ensure_ascii=False)
    if raw is not None:
        if raw[0] not in txt:
            return idx, 'muterr', 'raw anchor missing', ''
        txt = txt.replace(raw[0], raw[1], 1)
    cdir = os.path.join(out, f'case{idx % 64}_{os.getpid()}_{idx}')
    shutil.rmtree(cdir, ignore_errors=True)
    os.makedirs(cdir)
    open(os.path.join(cdir, 'project.pqd'), 'w', encoding='utf-8').write(txt)
    wav = os.path.join(cdir, 'out.wav')
    code, msg = render(cdir, wav, 1.5)
    st = ''
    if code == 0:
        w = wav_read(wav)
        if w is None:
            st = 'bad wav'
        else:
            bad = sum(1 for v in w[2] if not math.isfinite(v))
            peak = max((abs(v) for v in w[2] if math.isfinite(v)), default=0)
            st = f'nonfinite={bad} peak={peak:.3g}'
    keep = code not in (0, 1) or 'nonfinite=0' not in st and code == 0 or (exp == 'reject' and code == 0)
    if keep:
        shutil.copy(os.path.join(cdir, 'project.pqd'), os.path.join(out, f'keep_{idx}.pqd'))
    shutil.rmtree(cdir, ignore_errors=True)
    return idx, code, msg, st


res = {'cases': len(cases), 'accepted': 0, 'refused': 0, 'crash': 0, 'hang': 0, 'nonfinite': 0,
       'accepted_invalid': 0, 'rejected_valid': 0, 'no_path': 0, 'muterr': 0}
fails = []
import threading
sys.setrecursionlimit(60000)  # json.dumps of the 5000-deep case
threading.stack_size(256 * 1024 * 1024 - 4096)
with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
    only = os.environ.get('PQ_FUZZ_ONLY')  # "387,388": re-run just these cases
    todo = [int(x) for x in only.split(',')] if only else range(len(cases))
    for idx, code, msg, st in ex.map(run_case, todo):
        name, _, exp, _ = cases[idx]
        tag = ''
        if code == 'muterr':
            res['muterr'] += 1
            tag = 'MUTERR'
        elif code == 'hang':
            res['hang'] += 1
            tag = 'HANG'
        elif code == 0:
            res['accepted'] += 1
            if 'nonfinite=0' not in st:
                res['nonfinite'] += 1
                tag = 'NONFINITE'
            if exp == 'reject':
                res['accepted_invalid'] += 1
                tag += ' ACCEPTED-INVALID'
        elif code == 1:
            res['refused'] += 1
            if exp == 'accept':
                res['rejected_valid'] += 1
                tag = 'REJECTED-VALID'
            if not PATH_RE.search(msg):
                res['no_path'] += 1
                tag += ' NO-PATH'
        else:
            res['crash'] += 1
            tag = f'CRASH {code & 0xFFFFFFFF:#x}'
        line = f'{idx} [{exp}] {name[:120]}: code={code} {st} | {msg[:300]} {tag}'
        log.write(line + '\n')
        if tag:
            fails.append(line)
log.write('\n== flagged\n' + '\n'.join(fails) + '\n')
summary = f'laws mismatched={lawbad}/{len(expect)} migration mismatched={migbad}/5 groups failed={grpbad}/{len(names)} fuzz={res}'
log.write(summary + '\n')
log.close()
print(summary)
for f in fails:
    print(f)
