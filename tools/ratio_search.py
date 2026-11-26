#!/usr/bin/env python3
# Search delay pitch offsets (semitones) whose pairwise ratios stay far from
# small-integer ratios p/q (q <= 8). Used to pick kLateOffs/lt_sym and
# kEarlyOffs/er_sym. Run: python3 tools/ratio_search.py
import numpy as np, itertools
from fractions import Fraction
# penalty: for each pair ratio r>1, closeness to p/q with q<=8 (weighted by 1/q)
rats=sorted({Fraction(p,q) for q in range(1,9) for p in range(q,4*q+1)})
rv=np.array([float(r) for r in rats]); rq=np.array([r.denominator for r in rats])
def cost(offs):
    c=0
    for a,b in itertools.combinations(offs,2):
        r=2**(abs(a-b)/12)
        # distance in cents to nearest simple ratio, weighted
        d=np.abs(1200*np.log2(r/rv))
        c=max(c, np.max(np.exp(-d/6.0)*(1.0/rq)))   # worst pair; 6-cent scale
    return c
def late_set(a,b,c,s2): return [a,b,c,0,a+s2,b+s2,c+s2,s2]
def early_set(a,b,s2): return [a,b,0,a+s2,b+s2,s2]
print("current late (9,6,3, sym2.5):",round(cost(late_set(9,6,3,5)),3))
print("current early (8,4,0, sym2.42):",round(cost(early_set(8,4,4.84)),3))
rng=np.random.default_rng(1); best=(9,None)
for _ in range(60000):
    a=rng.uniform(7.5,10.5); b=rng.uniform(4.5,7); c=rng.uniform(1.8,3.8); s2=rng.uniform(3,5.5)
    if a-b<1.8 or b-c<1.8: continue
    k=cost(late_set(a,b,c,s2))
    if k<best[0]: best=(k,(a,b,c,s2))
print("best late:",round(best[0],4),[round(v,2) for v in best[1]])
bestE=(9,None)
for _ in range(60000):
    a=rng.uniform(13,17); b=rng.uniform(6,10); s2=rng.uniform(3,6)
    if a-b<3: continue
    k=cost(early_set(a,b,s2))
    if k<bestE[0]: bestE=(k,(a,b,s2))
print("best early:",round(bestE[0],4),[round(v,2) for v in bestE[1]])
