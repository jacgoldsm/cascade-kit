import numpy as np, torch, torch.nn.functional as F, nnlib, sys
recs = nnlib.load(sys.argv[1:])
codes, scal, win, fm, score = nnlib.prepare(recs)
n = len(codes); nv = n // 10
S = torch.from_numpy(scal); W = torch.from_numpy(win)
for cols in [[0], [0,1], [0,1,2,3]]:
    X = S[:, cols]; X = torch.cat([X, torch.ones(n,1)], 1)
    w = torch.zeros(X.shape[1], requires_grad=True)
    opt = torch.optim.LBFGS([w], max_iter=200)
    def clo():
        opt.zero_grad(); l = F.binary_cross_entropy_with_logits(X[:n-nv] @ w, W[:n-nv]); l.backward(); return l
    opt.step(clo)
    with torch.no_grad(): print(cols, F.binary_cross_entropy_with_logits(X[n-nv:] @ w, W[n-nv:]).item(), w.numpy())
sc = torch.from_numpy(score)
for k in [100, 200, 400, 800]:
    print('search score /', k, F.binary_cross_entropy_with_logits(sc[n-nv:]/k, W[n-nv:]).item())
