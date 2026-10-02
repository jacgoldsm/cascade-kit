# Variant trainer: extra global threat features appended to the scalar inputs.
import sys, time, numpy as np, torch, torch.nn as nn, torch.nn.functional as F, argparse
import nnlib
ap = argparse.ArgumentParser()
ap.add_argument('files', nargs='+'); ap.add_argument('--epochs', type=int, default=10); ap.add_argument('--bs', type=int, default=1024)
ap.add_argument('--lr', type=float, default=2e-3); ap.add_argument('--lam', type=float, default=0.25); ap.add_argument('--out', default='x.pt')
ap.add_argument('--extra', type=int, default=1)
a = ap.parse_args()
torch.manual_seed(0)
parts = [np.fromfile(f, dtype=nnlib.REC) for f in a.files]
trp = [p[:len(p) - len(p) // 10] for p in parts]; vap = [p[len(p) - len(p) // 10:] for p in parts]
recs = np.concatenate(trp + vap)
codes, scal, win, fm, score = nnlib.prepare(recs)
if a.extra:
    att = nnlib.attack_bits(codes)
    top = nnlib.TOPC[codes.astype(np.int64)]
    me_att = att & 1; op_att = (att >> 1) & 1
    f1 = ((top == 1) & (me_att == 1)).sum(1)            # opp stacks I can cover
    f2 = ((top == 0) & (op_att == 1)).sum(1)            # my stacks opp can cover
    f3 = ((top == 1) & (me_att == 1) & (op_att == 0)).sum(1)  # opp stacks I can take, undefended
    f4 = ((top == 0) & (op_att == 1) & (me_att == 0)).sum(1)  # my stacks hanging
    ex = np.stack([f1, f2, f3, f4], 1).astype(np.float32) / 10
    scal = np.concatenate([scal, ex], 1)
NS = scal.shape[1]
class NetX(nnlib.Net):
    def __init__(self):
        super().__init__(64, 32)
        self.sc = nn.Linear(NS, 64); self.skip = nn.Linear(NS, 1)
net = NetX()
n = len(codes); nv = sum(len(v) for v in vap)
C = torch.from_numpy(codes.astype(np.int64)); S = torch.from_numpy(scal); W = torch.from_numpy(win)
TGT = a.lam * W + (1 - a.lam) * torch.sigmoid(torch.from_numpy(score) / 170.0)
inv = torch.argsort(torch.from_numpy(nnlib.PERMS), 1)
opt = torch.optim.Adam(net.parameters(), lr=a.lr)
ntr = n - nv
sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs * (ntr // a.bs))
def evalv():
    with torch.no_grad():
        res = []; off = n - nv
        for v in vap:
            sl = slice(off, off + len(v)); off += len(v)
            res.append(round(F.binary_cross_entropy_with_logits(net(C[sl], S[sl]), W[sl]).item(), 4))
        return res
for ep in range(a.epochs):
    t0 = time.time(); perm = torch.randperm(ntr); tot = 0
    for b in range(ntr // a.bs):
        ix = perm[b * a.bs:(b + 1) * a.bs]
        k = np.random.randint(12)
        out = net(C[ix][:, inv[k]], S[ix])
        loss = F.binary_cross_entropy_with_logits(out, TGT[ix])
        opt.zero_grad(); loss.backward(); opt.step(); sched.step(); tot += loss.item()
    print(f'epoch {ep} train {tot / (ntr // a.bs):.4f} val {evalv()} {time.time() - t0:.0f}s', flush=True)
torch.save(net.state_dict(), a.out)
