# M3 views review probe: copies tests/views/fixtures/pianoroll-31edo into <out>/probe1 with a 1-tick note, huge-denominator
# ratios (524287/262144, 1048573/1048576), 13/8, a note 12 equaves up, a -9000 c note and very long track/clip names.
# Usage: python make_probe_outliers.py <repo> <outdir>; then
#   pqdaw --screenshot pianoroll-31edo <outdir>/probe1.png --project <outdir>/probe1
# Finding V-8: the roll's default view centres on the pitch extent's midpoint and shows none of the notes.
import os, sys
repo, out = sys.argv[1], sys.argv[2]
src = open(os.path.join(repo, 'tests/views/fixtures/pianoroll-31edo/project.pqd'), encoding='utf-8').read()
src = src.replace('"name": "1 MIDI"', '"name": "An extremely long track name that goes on and on and on WWWWWWWWWW"')
src = src.replace('"name": "Septimal"', '"name": "Septimal clip with a very long name WWWWWWWWWWWWWWWWWWWWWWWWWWW"')
extra = ''.join('            %s,\n' % n for n in [
    '{"id": 900, "start": 0, "length": 1, "pitch": {"degree": 3, "equave": 0}, "velocity": 100.0}',
    '{"id": 901, "start": 443520, "length": 221760, "pitch": {"ratio": "524287/262144"}, "velocity": 100.0}',
    '{"id": 902, "start": 665280, "length": 221760, "pitch": {"ratio": "13/8"}, "velocity": 100.0}',
    '{"id": 903, "start": 887040, "length": 221760, "pitch": {"degree": 0, "equave": 12}, "velocity": 100.0}',
    '{"id": 904, "start": 887040, "length": 221760, "pitch": {"cents": -9000.0}, "velocity": 100.0}',
    '{"id": 905, "start": 1108800, "length": 221760, "pitch": {"ratio": "1048573/1048576"}, "velocity": 100.0}'])
src = src.replace('"notes": [\n', '"notes": [\n' + extra, 1)
os.makedirs(os.path.join(out, 'probe1'), exist_ok=True)
open(os.path.join(out, 'probe1', 'project.pqd'), 'w', encoding='utf-8').write(src)
