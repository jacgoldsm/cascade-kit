import numpy as np, sys
fn = sys.argv[1]
a = np.fromfile(fn, dtype=np.uint8)
a = a[: len(a) // 72 * 72].reshape(-1, 72)
side = a[:, 61].astype(int); ply = a[:, 62].astype(int)
fm = a[:, 65].astype(np.int8).astype(float)
score = a[:, 68:70].copy().view(np.int16).reshape(-1).astype(float) / 100.0
sg = np.where(side == 0, 1.0, -1.0)
s = np.clip(score * sg, -30, 30)       # stm-perspective search score
# next-position score from this stm's perspective: -s_{t+1}
same = (ply[1:] == ply[:-1] + 1)
d = s[:-1][same] + s[1:][same]          # s_t + s_{t+1} (should be ~ 0 + tempo) -> difference s_t - (-s_{t+1})
print('mean(s_t + s_{t+1}) =', d.mean(), ' std =', d.std())
print('std s_t', s.std())
# by ply parity of side
for sd in (0, 1):
    m = same & (side[:-1] == sd)
    dd = (s[:-1] + s[1:])[m]
    print('side', sd, 'mean', dd.mean(), 'std', dd.std())
