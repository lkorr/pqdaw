# M5 review (document): mutates the audio fixture project (tests/views/fixtures/audio) field by field and renders each
# case with `pqdaw --project <dir> --render out.wav --seconds 3`, counting crashes (exit code not 0/1), hangs (timeout)
# and NaN/Inf samples in the rendered WAV of accepted cases.
# Usage: python -I fuzz_audio.py <repo root> <out dir> [random cases]
import copy, json, os, random, shutil, struct, subprocess, sys, math

root, out = sys.argv[1], sys.argv[2]
nrand = int(sys.argv[3]) if len(sys.argv) > 3 else 300
exe = os.path.join(root, 'build', 'release', 'src', 'app', 'pqdaw.exe')
fx = os.path.join(root, 'tests', 'views', 'fixtures', 'audio')
base = json.load(open(os.path.join(fx, 'project.pqd'), encoding='utf-8'))
os.makedirs(out, exist_ok=True)


def audio_clips(d):
    for t in d['tracks']:
        for c in t.get('clips', []):
            if c.get('kind') == 'audio':
                yield t, c


cases = []


def add(name, f):
    cases.append((name, f))


def clipset(field, val, which=0):
    def f(d):
        list(audio_clips(d))[which][1][field] = val
    return f


def sampleset(field, val, which=0):
    def f(d):
        d['samples'][which][field] = val
    return f


BIG = [0, -1, 1e-300, 1e300, -1e300, 9.1e15, -9.1e15, 2**52, 2**52 + 1, -(2**52), 2**63 - 1, -(2**63), 1e19]
for v in [-70.0, -70.0001, -1e9, 24.0, 24.0001, 1e30, -0.0]:
    add(f'gainDb={v}', clipset('gainDb', v))
for v in [4800.0, 4800.0001, -4800.0, -4800.5, 1e300]:
    add(f'cents={v}', clipset('transpose', {'cents': v}))
for v in [1024, 1025, -1024, -1025, 2**31 - 1, -(2**31)]:
    add(f'steps={v}', clipset('transpose', {'steps': v}))
for v in ['0/1', '1/0', '-1/2', '0/0', '16/1', '17/1', '1/16', '1/17', '99999999999999999999/1', '3/2/1', ' 3/2', '3',
          '4294967297/4294967296', '18446744073709551615/18446744073709551614']:
    add(f'ratio={v}', clipset('transpose', {'ratio': v}))
add('transpose two keys', clipset('transpose', {'cents': 1.0, 'steps': 1}))
add('transpose empty', clipset('transpose', {}))
for v in [50.0, 50.0001, -50.0001, 1e30]:
    add(f'detune={v}', clipset('detuneCents', v))
for v in [-1, 0, 2**52, 2**52 + 1, 2**62, 1774080 * 10]:
    add(f'fadeIn.length={v}', clipset('fadeIn', {'length': v, 'curve': 0.0}))
    add(f'fadeOut.length={v}', clipset('fadeOut', {'length': v, 'curve': 0.0}))
for v in [1.0, -1.0, 1.0001, -1.5, 1e30]:
    add(f'fadeIn.curve={v}', clipset('fadeIn', {'length': 1000, 'curve': v}))
for v in BIG:
    add(f'contentStartFrame={v}', clipset('contentStartFrame', v))
    add(f'contentStartFrame(rev clip)={v}', clipset('contentStartFrame', v, 2))
for v in [0, -1, 1e-10, 1e-9, 1e8, 1e8 + 1, 1e300, 0.5e-9]:
    add(f'seconds={v}', clipset('seconds', v))
for v in [0, -1, 0.99, 1, 1000, 1000.1, 1e300]:
    add(f'sourceBpm(repitch)={v}', clipset('sourceBpm', v, 3))
for v in [-(2**52), -(2**52) - 1, 2**52, 2**52 - 1, -1]:
    add(f'start={v}', clipset('start', v))
    add(f'start(repitch)={v}', clipset('start', v, 3))
for v in [0, 1, 2**52, 2**52 + 1, -5]:
    add(f'length(repitch)={v}', clipset('length', v, 3))
    add(f'length(off; derived)={v}', clipset('length', v, 0))
for v in [-(2**52), 2**52, -887040 * 1000, 887040 * 1000]:
    add(f'contentStart(repitch)={v}', clipset('contentStart', v, 3))
add('loop inverted', clipset('loop', {'enabled': True, 'start': 1000, 'end': 10}, 3))
add('loop empty', clipset('loop', {'enabled': True, 'start': 10, 'end': 10}, 3))
add('loop 1 tick', clipset('loop', {'enabled': True, 'start': 10, 'end': 11}, 3))
add('loop huge', clipset('loop', {'enabled': True, 'start': -(2**52), 'end': 2**52}, 3))
add('loop far right of contentStart', clipset('loop', {'enabled': True, 'start': 2**51, 'end': 2**51 + 3}, 3))
add('loop neg', clipset('loop', {'enabled': True, 'start': -887040 * 4, 'end': -887040}, 3))
for v in [-1, 0, 2**52, 2**52 + 1, 10**15]:
    add(f'crossfadeIn={v}', clipset('crossfadeIn', v, 1))
    add(f'crossfadeIn(no neighbour)={v}', clipset('crossfadeIn', v, 0))
add('warp bogus', clipset('warp', 'beats'))
add('sample unknown', clipset('sample', 999))
add('sample = track id', clipset('sample', 10))
add('sample = missing (5)', clipset('sample', 5))
add('kind=midi on audio clip', clipset('kind', 'midi'))
for v in [0, -1, 1, 2**63 - 1, 1e19, 4.5]:
    add(f'frames={v}', sampleset('frames', v))
for v in [0, -1, 1e-3, 0.999e-3, 1e-4, 1e7, 1.0000001e7, 1e300, 1]:
    add(f'rate={v}', sampleset('rate', v))
    add(f'rate(repitch sample)={v}', sampleset('rate', v, 1))
for v in [0, 1, 1024, 1025, 2**32, -1, 2**31]:
    add(f'channels={v}', sampleset('channels', v))
for v in [-1, 0, 2**63, 2**64]:
    add(f'size={v}', sampleset('size', v))
for v in ['', 'ABCDEF0123456789', '0', '00c3f9764c29545', 'zz']:
    add(f'hash={v!r}', sampleset('hash', v))
for v in ['', '..\\..\\..\\Windows\\win.ini', '../../../../../../Windows/System32/drivers/etc/hosts', 'C:/Windows/win.ini',
          'C:/Windows', '//127.0.0.1/C$/Windows/win.ini', '\\\\?\\C:\\Windows\\win.ini', 'NUL', 'CON',
          'C:relative.wav', '/rooted.wav', 'samples/beat.wav\u0000x', 'a' * 5000]:
    add(f'path={v[:40]!r}', sampleset('path', v))


def dup_sample(d):
    d['samples'].insert(1, copy.deepcopy(d['samples'][0]))


add('duplicate sample id', dup_sample)
add('samples unsorted', lambda d: d['samples'].reverse())
add('samples empty (clips refer)', lambda d: d.__setitem__('samples', []))
add('samples key absent', lambda d: d.pop('samples'))
add('sample id = clip id', lambda d: d['samples'][2].__setitem__('id', 20))


def audio_on_midi(d):
    mt = [t for t in d['tracks'] if t['kind'] == 'midi'][0]
    mt['clips'].append(copy.deepcopy(list(audio_clips(d))[0][1]) | {'id': 999, 'start': 887040 * 40})


add('audio clip on midi track', audio_on_midi)


def note_on_audio(d):
    mt = [t for t in d['tracks'] if t['kind'] == 'midi'][0]
    nc = [c for c in mt['clips'] if 'kind' not in c][0]
    at = [t for t in d['tracks'] if t['kind'] == 'audio'][0]
    at['clips'].append(copy.deepcopy(nc) | {'id': 998, 'start': 887040 * 40})


add('note clip on audio track', note_on_audio)


def overlap(d):
    t, c = list(audio_clips(d))[0]
    c2 = copy.deepcopy(c)
    c2['id'] = 997
    c2['start'] = c['start'] + 1000
    t['clips'].insert(1, c2)


add('overlapping audio clips', overlap)


def overlap_many(d):
    t, c = list(audio_clips(d))[0]
    for k in range(200):
        c2 = copy.deepcopy(c)
        c2['id'] = 2000 + k
        c2['start'] = c['start'] + 1 + k
        t['clips'].insert(1 + k, c2)


add('200 stacked audio clips', overlap_many)


def same_start(d):
    t, c = list(audio_clips(d))[0]
    c2 = copy.deepcopy(c)
    c2['id'] = 996
    t['clips'].insert(1, c2)


add('two audio clips same start', same_start)
add('crossfade with overlapping neighbour', lambda d: d['tracks'][0]['clips'][1].__setitem__('start', 887040 - 5000))
add('schema 6 with audio', lambda d: d.__setitem__('schemaVersion', 6))


def audio_in_group(d):
    at = d['tracks'][0]
    d['tracks'][0] = {'id': 3000, 'kind': 'group', 'name': 'G', 'colour': '#112233', 'active': True, 'tuning': 'inherit',
                      'mixer': at['mixer'], 'lanes': [], 'devices': [], 'children': [at]}


add('audio track inside group', audio_in_group)


def audio_return(d):
    t = copy.deepcopy(d['tracks'][0])
    t['id'] = 3001
    for i, c in enumerate(t['clips']):
        c['id'] = 3100 + i
    d['returns'].append(t)


add('audio track in returns', audio_return)


def steps_private(d):
    cl = list(audio_clips(d))[0][1]
    cl['transpose'] = {'steps': 1024}
    cl['tuning'] = {'private': {'kind': 'edo', 'n': 1, 'equave': '1000/1', 'referenceHz': 1.0}}


add('steps 1024 in 1-edo of 1000/1 (private)', steps_private)

rng = random.Random(5)
fields_clip = [('gainDb', [-70.0, 24.0, -1e9, 0.0]), ('transpose', [{'cents': 4800.0}, {'steps': -1024}, {'ratio': '1/16'}]),
               ('detuneCents', [50.0, -50.0]), ('reverse', [True, False]), ('warp', ['off', 'rePitch']),
               ('contentStartFrame', [-9e15, 9e15, -1e6, 1e12, 0.5]), ('seconds', [1e-9, 1e8, 0.001, 3.0]),
               ('sourceBpm', [1.0, 1000.0]), ('crossfadeIn', [0, 2**52, 1]),
               ('fadeIn', [{'length': 2**52, 'curve': 1.0}, {'length': 1, 'curve': -1.0}]),
               ('fadeOut', [{'length': 2**52, 'curve': -1.0}]), ('start', [-(2**52), 2**52 - 10, 0, 443520]),
               ('contentStart', [-(2**52), 2**52, -1]), ('length', [1, 2**52, 887040]),
               ('loop', [{'enabled': True, 'start': -(2**52), 'end': 2**52}, {'enabled': True, 'start': 0, 'end': 1},
                         {'enabled': True, 'start': 2**52 - 1, 'end': 2**52}])]
fields_sample = [('rate', [1e-3, 1e7, 44100.0]), ('frames', [1, 2**62]), ('channels', [1, 1024])]
for k in range(nrand):
    muts = []
    for _ in range(rng.randint(1, 5)):
        if rng.random() < 0.8:
            f, vals = rng.choice(fields_clip)
            muts.append(('c', rng.randrange(5), f, rng.choice(vals)))
        else:
            f, vals = rng.choice(fields_sample)
            muts.append(('s', rng.randrange(2), f, rng.choice(vals)))

    def f(d, muts=muts):
        cl = list(audio_clips(d))
        for kind, i, fld, v in muts:
            if kind == 'c':
                cl[i % len(cl)][1][fld] = v
            else:
                d['samples'][i][fld] = v
    add(f'random {k}: ' + '; '.join(f'{m[0]}{m[1]}.{m[2]}={str(m[3])[:30]}' for m in muts), f)

sdir = os.path.join(out, 'samples_abs')
os.makedirs(sdir, exist_ok=True)
for n in ('beat.wav', 'vox.wav'):
    shutil.copy(os.path.join(fx, 'samples', n), os.path.join(sdir, n))


def wav_stats(path):
    try:
        b = open(path, 'rb').read()
    except OSError:
        return 'no wav'
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
        return 'bad wav'
    if fmt[0] not in (3, 0xFFFE):
        return f'fmt {fmt[0]}'
    n = len(data) // 4
    vals = struct.unpack(f'<{n}f', data[:n * 4])
    bad = sum(1 for v in vals if not math.isfinite(v))
    peak = max((abs(v) for v in vals if math.isfinite(v)), default=0)
    return f'nonfinite={bad} peak={peak:.3g}'


res = {'accepted': 0, 'refused': 0, 'crash': 0, 'hang': 0, 'nonfinite': 0}
only = os.environ.get('PQ_FUZZ_ONLY')
log = open(os.path.join(out, 'fuzz_log.txt'), 'w', encoding='utf-8')
for idx, (name, mut) in enumerate(cases):
    if only and str(idx) not in only.split(','):
        continue
    d = copy.deepcopy(base)
    for s in d['samples']:
        p = s['path']
        if p.startswith('samples/') and p != 'samples/missing.wav':
            s['path'] = os.path.join(sdir, p[len('samples/'):]).replace('\\', '/')
    try:
        mut(d)
    except Exception as e:
        log.write(f'{idx} {name}: MUTATION ERROR {e}\n')
        continue
    cdir = os.path.join(out, 'case')
    shutil.rmtree(cdir, ignore_errors=True)
    os.makedirs(cdir)
    open(os.path.join(cdir, 'project.pqd'), 'w', encoding='utf-8').write(json.dumps(d, indent=2, ensure_ascii=False))
    wav = os.path.join(out, 'case.wav')
    if os.path.exists(wav):
        os.remove(wav)
    try:
        r = subprocess.run([exe, '--project', cdir, '--render', wav, '--seconds', '3'], capture_output=True, timeout=60)
    except subprocess.TimeoutExpired:
        res['hang'] += 1
        log.write(f'{idx} {name}: HANG\n')
        log.flush()
        shutil.copy(os.path.join(cdir, 'project.pqd'), os.path.join(out, f'hang_{idx}.pqd'))
        continue
    code = r.returncode
    msg = (r.stderr or r.stdout).decode('utf-8', 'replace').strip().splitlines()
    msg = msg[-1] if msg else ''
    if code == 0:
        res['accepted'] += 1
        st = wav_stats(wav)
        if 'nonfinite=0' not in st:
            res['nonfinite'] += 1
            shutil.copy(os.path.join(cdir, 'project.pqd'), os.path.join(out, f'nonfinite_{idx}.pqd'))
        log.write(f'{idx} {name}: OK {st}\n')
    elif code == 1:
        res['refused'] += 1
        log.write(f'{idx} {name}: refused: {msg}\n')
    else:
        res['crash'] += 1
        log.write(f'{idx} {name}: CRASH code={code & 0xFFFFFFFF:#x} {msg}\n')
        shutil.copy(os.path.join(cdir, 'project.pqd'), os.path.join(out, f'crash_{idx}.pqd'))
    log.flush()
print(len(cases), 'cases', res)
log.write(f'{len(cases)} cases {res}\n')
log.close()
