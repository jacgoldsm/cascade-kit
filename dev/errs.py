import numpy as np, sys, struct
SP = sys.argv[1]; net = sys.argv[2]; fn = sys.argv[3]
N = 61
R = 4
cells = []; idx = {}
for r in range(-R, R + 1):
    for q in range(max(-R, -R - r), min(R, R - r) + 1):
        idx[(q, r)] = len(cells); cells.append((q, r))
a = np.fromfile(fn, dtype=np.uint8); a = a[: len(a) // 72 * 72].reshape(-1, 72)
rng = np.random.default_rng(3)
a = a[rng.choice(len(a), size=60000, replace=False)]
code = a[:, :61].astype(np.int64)
side = a[:, 61].astype(int); ply = a[:, 62].astype(int); capW = a[:, 63].astype(int); capB = a[:, 64].astype(int)
score = a[:, 68:70].copy().view(np.int16).reshape(-1).astype(float) / 100.0
fm = a[:, 65].astype(np.int8).astype(float)
sat = np.abs(score) >= 80
score = np.where(sat, fm, score)
h = np.floor(np.log2(np.maximum(code, 1))).astype(int)
nonempty = code > 1; top = code & 1
own = nonempty & (top == side[:, None]); opp = nonempty & (top != side[:, None])
sg = np.where(side == 0, 1.0, -1.0)
y = np.clip(score * sg, -30, 30)
capOwn = np.where(side == 0, capW, capB); capOpp = np.where(side == 0, capB, capW)
mnow = own.sum(1) - opp.sum(1) + capOwn - capOpp
b = open(net, 'rb').read(); H = struct.unpack('i', b[4:8])[0]
f = np.frombuffer(b[8:], dtype=np.float32); o = 0
W1 = f[o:o + N * 64 * H].reshape(N * 64, H); o += N * 64 * H
B1 = f[o:o + H]; o += H
WD = f[o:o + 3 * H].reshape(3, H); o += 3 * H
V = f[o:o + H]; o += H; C0 = f[o]
FLP = np.zeros(64, int)
for c in range(2, 64):
    hh = int(np.floor(np.log2(c))); FLP[c] = c ^ ((1 << hh) - 1)
cd = code.copy(); fl = side == 1
cd[fl] = FLP[np.clip(cd[fl], 0, 63)]; cd = np.clip(cd, 0, 63)
rows = W1[np.arange(N)[None, :] * 64 + cd] * (code > 1)[..., None]
pre = rows.sum(1) + B1 + ((capOwn - capOpp) * 0.1)[:, None] * WD[0] + (ply / 150.0)[:, None] * WD[1] + ((own.sum(1) - opp.sum(1)) * 0.1)[:, None] * WD[2]
pred = mnow + np.maximum(pre, 0) @ V + C0
err = y - pred
print('mse', (err ** 2).mean(), ' by |mnow-y|: corr(err, y-mnow) =', np.corrcoef(err, y - mnow)[0, 1])
for lo in range(0, 150, 30):
    m = (ply >= lo) & (ply < lo + 30)
    print('ply %3d-%3d mse %.2f  std(y-mnow) %.2f' % (lo, lo + 30, (err[m] ** 2).mean(), (y - mnow)[m].std()))
# show worst mid-game errors
mid = np.where((ply >= 40) & (ply < 100))[0]
worst = mid[np.argsort(-np.abs(err[mid]))[:4]]
def show(i):
    print('--- ply', ply[i], 'stm', 'W' if side[i] == 0 else 'B', 'search(stm)=%.1f pred=%.1f mnow=%d' % (y[i], pred[i], mnow[i]))
    rowstart = 0
    for r in range(-R, R + 1):
        n = 9 - abs(r)
        line = ' ' * abs(r) * 3
        for k in range(n):
            c = rowstart + k
            cdv = code[i, c]
            if cdv <= 1: s = '.'
            else:
                hh = int(np.floor(np.log2(cdv))); s = ''.join('b' if (cdv >> (hh - 1 - j)) & 1 else 'w' for j in range(hh))
                s = s[:-1] + s[-1].upper()
            line += s.ljust(6)
        print(line); rowstart += n
for i in worst: show(i)
