import sys, time, numpy as np, torch, torch.nn.functional as F, argparse
import nnlib
ap = argparse.ArgumentParser()
ap.add_argument('files', nargs='+'); ap.add_argument('--h1', type=int, default=64); ap.add_argument('--h2', type=int, default=32)
ap.add_argument('--epochs', type=int, default=10); ap.add_argument('--bs', type=int, default=1024); ap.add_argument('--lr', type=float, default=2e-3)
ap.add_argument('--out', default='net.pt'); ap.add_argument('--lam', type=float, default=1.0, help='weight on result vs search score')
ap.add_argument('--wd', type=float, default=0.0)
a = ap.parse_args()
torch.manual_seed(0)
recs = nnlib.load(a.files)
codes, scal, win, fm, score = nnlib.prepare(recs)
n = len(codes); nv = n // 10
# validation = last 10% (whole games mostly)
tr = slice(0, n - nv); va = slice(n - nv, n)
C = torch.from_numpy(codes.astype(np.int64)); S = torch.from_numpy(scal); W = torch.from_numpy(win)
SC = torch.sigmoid(torch.from_numpy(score) / 400.0)
TGT = a.lam * W + (1 - a.lam) * SC
P = torch.from_numpy(nnlib.PERMS)
inv = torch.argsort(P, 1)  # new[j] = old[inv[j]]
net = nnlib.Net(a.h1, a.h2)
opt = torch.optim.Adam(net.parameters(), lr=a.lr, weight_decay=a.wd)
def evalv():
    with torch.no_grad():
        out = net(C[va], S[va])
        return F.binary_cross_entropy_with_logits(out, W[va]).item(), ((out > 0).float() == W[va]).float().mean().item()
print('train', n - nv, 'val', nv, 'base val', evalv(), flush=True)
ntr = n - nv
sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, a.epochs * (ntr // a.bs))
for ep in range(a.epochs):
    t0 = time.time(); perm = torch.randperm(ntr); tot = 0
    for b in range(ntr // a.bs):
        ix = perm[b * a.bs:(b + 1) * a.bs]
        k = np.random.randint(12)
        cb = C[ix][:, inv[k]]
        out = net(cb, S[ix])
        loss = F.binary_cross_entropy_with_logits(out, TGT[ix])
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
        tot += loss.item()
    print(f'epoch {ep} train {tot / (ntr // a.bs):.4f} val {evalv()} {time.time() - t0:.0f}s', flush=True)
torch.save(net.state_dict(), a.out)
