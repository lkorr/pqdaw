"""M3 review: derive probe projects from tests/app/fixtures/tuning_automation (run from the worktree root)."""
import copy, json, os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
D = os.path.dirname(os.path.abspath(__file__))
src = json.load(open(os.path.join(ROOT, 'tests/app/fixtures/tuning_automation/project.pqd')))


def save(name, doc):
    os.makedirs(os.path.join(D, name), exist_ok=True)
    json.dump(doc, open(os.path.join(D, name, 'project.pqd'), 'w'), indent=1)


def mos(doc):
    return [x for x in doc['tunings'] if x['id'] == 4][0]['def']


# A: region hint 5L2s, automation 700 -> 720 over 1 s, then hold at 720 (the collapsed edge of 5L2s)
a = copy.deepcopy(src)
mos(a)['region'] = {'perPeriodSize': 7, 'lowCents': 4 / 7 * 1200, 'highCents': 720.0}
a['tracks'][0]['lanes'][0]['points'] = [{'id': 8, 'tick': 0, 'value': 700.0, 'curve': 0.0},
                                        {'id': 9, 'tick': 443520, 'value': 720.0, 'curve': 0.0}]
save('proj_boundary_auto', a)

# B: same tuning, generator 720 in the definition, no automation (static resolution)
b = copy.deepcopy(a)
mos(b)['generator'] = 720.0
b['tracks'][0]['lanes'] = []
save('proj_boundary_static', b)

# C: the original fixture, but the clip overrides to tuning 4 while the track follows tuning 1 (12-EDO):
#    the generator automation of tuning 4 should move the note (SPEC 3.2); Q105.1 says it plays static.
c = copy.deepcopy(src)
c['tracks'][0]['tuning'] = {'main': 1}
c['tracks'][0]['clips'][0]['tuning'] = {'main': 4}
save('proj_clip_override', c)

# D: a 3/2 Ratio note anchored to the Degree note (id 5), the anchor disabled so only the anchored note sounds.
#    SPEC 3.1: Ratio "relative to an anchor (another note)"; the anchor follows the generator automation.
d = copy.deepcopy(src)
notes = d['tracks'][0]['clips'][0]['notes']
notes[0]['disabled'] = True
notes.append({'id': 10, 'start': 0, 'length': 1108800,
              'pitch': {'ratio': '3/2', 'anchor': 'note', 'anchorId': 5}, 'velocity': 110.0})
save('proj_anchor_ratio', d)

# E: reference automation 261.626 -> 300 Hz over bar 1 on tuning 4, with one Cents note (+200 c) and nothing else.
e = copy.deepcopy(src)
e['tracks'][0]['lanes'][0]['param'] = {'kind': 'tuningReference', 'owner': 4}
e['tracks'][0]['lanes'][0]['points'] = [{'id': 8, 'tick': 0, 'value': 261.626, 'curve': 0.0},
                                        {'id': 9, 'tick': 887040, 'value': 300.0, 'curve': 0.0}]
e['tracks'][0]['clips'][0]['notes'][0]['pitch'] = {'cents': 200.0}
save('proj_ref_auto_cents', e)
e2 = copy.deepcopy(e)
e2['tracks'][0]['clips'][0]['notes'][0]['pitch'] = {'degree': 1, 'equave': 0}
save('proj_ref_auto_degree', e2)
