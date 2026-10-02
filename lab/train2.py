import sys, time, numpy as np, torch, torch.nn.functional as F, argparse
import nnlib
ap = argparse.ArgumentParser()
ap.add_argument('files', nargs='+'); ap.add_argument('--h1', type=int, default=64); ap.add_argument('--h2', type=int, default=32)
ap.add_argument('--epochs', type=int, default=10); ap.add_argument('--bs', type=int, default=1024); ap.add_argument('--lr', type=float, default=2e-3)
ap.add_argument('--out', default='net.pt'); ap.add_argument('--lam', type=float, default=1.0, help='weight on result vs search score')
ap.add_argument('--wd', type=float, default=0.0); ap.add_argument('--v2', action='store_true'); ap.add_argument('--init', default=None)
a = ap.parse_args()
torch.manual_seed(0)
parts = [np.fromfile(f, dtype=nnlib.REC) for f in a.files]
trp = [p[:len(p) - len(p) // 10] for p in parts]; vap = [p[len(p) - len(p) // 10:] for p in parts]
recs = np.concatenate(trp + vap)
codes, scal, win, fm, score = nnlib.prepare(recs)
n = len(codes); nv = sum(len(v) for v in vap)
C = torch.from_numpy(codes.astype(np.int64)); S = torch.from_numpy(scal); W = torch.from_numpy(win)
A = torch.from_numpy(nnlib.attack_bits(codes.astype(np.int64)).astype(np.int64)) if a.v2 else torch.zeros_like(C)
SC = torch.sigmoid(torch.from_numpy(score) / 400.0)
TGT = a.lam * W + (1 - a.lam) * SC
P = torch.from_numpy(nnlib.PERMS)
inv = torch.argsort(P, 1)  # new[j] = old[inv[j]]
net = (nnlib.Net2 if a.v2 else nnlib.Net)(a.h1, a.h2)
if a.init: net.load_state_dict(torch.load(a.init))
fwd = (lambda c, s_, at: net(c, s_, at)) if a.v2 else (lambda c, s_, at: net(c, s_))
net.emb.sparse = True
dense = [p for n_, p in net.named_parameters() if not n_.startswith('emb.')]
opt = torch.optim.Adam(dense, lr=a.lr, weight_decay=a.wd)
opt_e = torch.optim.SparseAdam([net.emb.weight], lr=a.lr)
def evalv():
    with torch.no_grad():
        res = []
        off = n - nv
        for v in vap:
            sl = slice(off, off + len(v)); off += len(v)
            out = fwd(C[sl], S[sl], A[sl])
            res.append(round(F.binary_cross_entropy_with_logits(out, W[sl]).item(), 4))
        return res
print('train', n - nv, 'val', nv, 'base val', evalv(), flush=True)
ntr = n - nv
sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs * (ntr // a.bs))
sched_e = torch.optim.lr_scheduler.CosineAnnealingLR(opt_e, a.epochs * (ntr // a.bs))
for ep in range(a.epochs):
    t0 = time.time(); perm = torch.randperm(ntr); tot = 0
    for b in range(ntr // a.bs):
        ix = perm[b * a.bs:(b + 1) * a.bs]
        k = np.random.randint(12)
        cb = C[ix][:, inv[k]]
        out = fwd(cb, S[ix], A[ix][:, inv[k]])
        loss = F.binary_cross_entropy_with_logits(out, TGT[ix])
        opt.zero_grad(); opt_e.zero_grad(); loss.backward(); opt.step(); opt_e.step(); sched.step(); sched_e.step()
        tot += loss.item()
    print(f'epoch {ep} train {tot / (ntr // a.bs):.4f} val {evalv()} {time.time() - t0:.0f}s', flush=True)
torch.save(net.state_dict(), a.out)
