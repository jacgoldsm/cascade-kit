import numpy as np, sys, struct
SP = sys.argv[1]
N = 61
# geometry
R = 4
cells = []
idx = {}
for r in range(-R, R + 1):
    for q in range(max(-R, -R - r), min(R, R - r) + 1):
        idx[(q, r)] = len(cells); cells.append((q, r))
DQ = [1, 1, 0, -1, -1, 0]; DR = [0, -1, -1, 0, 1, 1]
nb = -np.ones((N, 6), int)
for c, (q, r) in enumerate(cells):
    for d in range(6):
        t = (q + DQ[d], r + DR[d])
        if t in idx: nb[c, d] = idx[t]
# landing cell tables L[c][d][h] for h=1..5 ; -1 if illegal first step
L = -np.ones((N, 6, 6), int)
for c in range(N):
    for d0 in range(6):
        if nb[c, d0] < 0: continue
        pos, d = c, d0
        for i in range(1, 6):
            if nb[pos, d] < 0: d = (d + 3) % 6
            pos = nb[pos, d]
            L[c, d0, i] = pos
# load data
recs = []
for fn in sys.argv[2:]:
    a = np.fromfile(fn, dtype=np.uint8)
    a = a[: len(a) // 72 * 72].reshape(-1, 72)
    recs.append(a)
a = np.concatenate(recs)
rng = np.random.default_rng(1)
sel = rng.choice(len(a), size=min(400000, len(a)), replace=False)
a = a[sel]
code = a[:, :61].astype(np.int64)
side = a[:, 61].astype(int); ply = a[:, 62].astype(int); capW = a[:, 63].astype(int); capB = a[:, 64].astype(int)
fm = a[:, 65].astype(np.int8).astype(float)
score = a[:, 68:70].copy().view(np.int16).reshape(-1).astype(float) / 100.0
h = np.floor(np.log2(np.maximum(code, 1))).astype(int)  # height (code has sentinel); empty->0
nonempty = code > 1
top = code & 1
# stm-relative: own = top == side
own = nonempty & (top == side[:, None])
opp = nonempty & (top != side[:, None])
sgn = np.where(side == 0, 1.0, -1.0)
y = np.clip(score * sgn, -30, 30)  # stm perspective search score
capOwn = np.where(side == 0, capW, capB); capOpp = np.where(side == 0, capB, capW)
mnow = own.sum(1) - opp.sum(1) + capOwn - capOpp
# net prediction
def loadnet(path):
    b = open(path, 'rb').read()
    H = struct.unpack('i', b[4:8])[0]
    f = np.frombuffer(b[8:], dtype=np.float32)
    o = 0
    W1 = f[o:o + N * 64 * H].reshape(N * 64, H); o += N * 64 * H
    B1 = f[o:o + H]; o += H
    WD = f[o:o + 3 * H].reshape(3, H); o += 3 * H
    V = f[o:o + H]; o += H
    C0 = f[o]
    return H, W1, B1, WD, V, C0
H, W1, B1, WD, V, C0 = loadnet(SP + '/nets/n2a.net')
FLP = np.zeros(64, int)
for c in range(2, 64):
    hh = int(np.floor(np.log2(c))); FLP[c] = c ^ ((1 << hh) - 1)
pred = np.zeros(len(a))
for s0 in range(0, len(a), 20000):
    sl = slice(s0, s0 + 20000)
    cd = code[sl].copy()
    flip = side[sl] == 1
    cd[flip] = FLP[np.clip(cd[flip], 0, 63)]
    cd = np.clip(cd, 0, 63)
    cell = np.arange(N)[None, :] * 64 + cd
    act = (code[sl] > 1)
    rows = W1[cell] * act[..., None]
    pre = rows.sum(1) + B1
    xc = (capOwn[sl] - capOpp[sl]) * 0.1; xp = ply[sl] / 150.0; xd = (own[sl].sum(1) - opp[sl].sum(1)) * 0.1
    pre = pre + xc[:, None] * WD[0] + xp[:, None] * WD[1] + xd[:, None] * WD[2]
    r = np.maximum(pre, 0) @ V + C0
    pred[sl] = mnow[sl] + r
print('records', len(a))
def r2(Xcols, y):
    X = np.column_stack(Xcols + [np.ones(len(y))])
    w, *_ = np.linalg.lstsq(X, y, rcond=None)
    res = y - X @ w
    return (res ** 2).mean(), w
print('var(y)', y.var(), ' mse(mnow)', ((y - mnow) ** 2).mean(), ' mse(net pred)', ((y - pred) ** 2).mean())
m0, w0 = r2([pred], y)
print('mse(a*pred+b)', m0, w0)
# attack maps: A_own[c] = #(own moves landing on c), A_opp similarly (opp moves if opp to move)
def attack(ctrl):
    A = np.zeros((len(a), N), np.int16)
    for c in range(N):
        for d in range(6):
            if nb[c, d] < 0: continue
            for hh in range(1, 6):
                m = ctrl[:, c] & (h[:, c] == hh)
                if not m.any(): continue
                np.add.at(A, (np.nonzero(m)[0], L[c, d, hh]), 1)
    return A
Aown = attack(own); Aopp = attack(opp)
hh = h
feats = {}
hit_opp_by_own = (opp & (Aown > 0))
hit_own_by_opp = (own & (Aopp > 0))
feats['n_opp_stacks_hit_by_own'] = hit_opp_by_own.sum(1)
feats['n_own_stacks_hit_by_opp'] = hit_own_by_opp.sum(1)
feats['n_own_moves'] = Aown.sum(1)
feats['n_opp_moves'] = Aopp.sum(1)
for k in (3, 4, 5):
    feats['own_stk_h%d_hit_by_opp' % k] = (own & (hh == k) & (Aopp > 0)).sum(1)
    feats['opp_stk_h%d_hit_by_own' % k] = (opp & (hh == k) & (Aown > 0)).sum(1)
feats['n_own'] = own.sum(1); feats['n_opp'] = opp.sum(1)
cols = [pred] + [v.astype(float) for v in feats.values()]
m1, w1 = r2(cols, y)
print('mse(pred + attack feats)', m1)
for n, w in zip(['pred'] + list(feats.keys()), w1): print('  %-28s %.3f' % (n, w))
# also: quantity "max over own moves of (stacks flipped)" is expensive; skip
