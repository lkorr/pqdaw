"""M4 RT review: does a project with more device params than ProjectLink's 16384 param slots play its stored values?

Every track: Operator -> Utility (mute = 1) -> EQ Eight -> Saturator, a held chord. Every track is muted by its own
Utility, so the render must be silent. A track whose Utility `mute` got no param slot plays the stock default
(mute off) and is heard.

usage: python -I gen_param_capacity.py <out_root> [tracks...]   (default 40 70)
then:  pqdaw --project <out_root>/cap<N> --render cap<N>.wav --seconds 1   and compare the peak level.
"""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_device_projects as g  # noqa: E402


def project(n):
    p = g.project(n, "op", "held", 2)
    for t in p["tracks"]:
        for d in t["devices"]:
            if d["patch"] == "utility":
                d["params"] = {"mute": 1.0}
    return p


root = sys.argv[1]
for n in [int(x) for x in (sys.argv[2:] or ["40", "70"])]:
    d = os.path.join(root, f"cap{n}")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "project.pqd"), "w") as f:
        json.dump(project(n), f, indent=1)
    print(d)
