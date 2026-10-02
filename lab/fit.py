import numpy as np, sys
fn, nf = sys.argv[1], int(sys.argv[2])
d = np.fromfile(fn, dtype=np.float32).reshape(-1, 3 + nf).astype(np.float64)
tm, tw, sc, X = d[:,0], d[:,1], d[:,2], d[:,3:]
n = len(X); idx = np.random.RandomState(0).permutation(n); tr, te = idx[:int(.85*n)], idx[int(.85*n):]
names = sys.argv[3].split(',') if len(sys.argv) > 3 else [str(i) for i in range(nf)]
# linear regression on margin
lam = 1.0
A = X[tr].T @ X[tr] + lam*np.eye(nf); w = np.linalg.solve(A, X[tr].T @ tm[tr])
pred = X @ w
print("margin MSE base(mat only):", np.mean((tm[te]-X[te,0]*np.dot(X[tr,0],tm[tr])/np.dot(X[tr,0],X[tr,0]))**2), "full:", np.mean((tm[te]-pred[te])**2), "var", np.var(tm[te]))
# logistic on win via Newton
def logfit(Xtr, y, iters=30, lam=1.0):
    w = np.zeros(Xtr.shape[1])
    for _ in range(iters):
        z = Xtr @ w; p = 1/(1+np.exp(-z))
        g = Xtr.T @ (p - y) + lam*w
        Hm = (Xtr * (p*(1-p))[:,None]).T @ Xtr + lam*np.eye(len(w))
        w -= np.linalg.solve(Hm, g)
    return w
wl = logfit(X[tr], tw[tr])
def ll(w, X, y):
    p = np.clip(1/(1+np.exp(-(X@w))), 1e-9, 1-1e-9); return -np.mean(y*np.log(p)+(1-y)*np.log(1-p))
wm = logfit(X[tr][:, :2], tw[tr])
print("logloss mat+bias:", ll(wm, X[te][:, :2], tw[te]), "full:", ll(wl, X[te], tw[te]))
print("search-score corr with win: ", ll(np.array([1.0]), (sc[te]*0.5)[:,None], tw[te]))
scale = 1/wl[0]
for i in range(nf):
    print(f"{i:3d} {names[i] if i < len(names) else i:>12s} lin {w[i]:8.3f}  logit/mat {wl[i]*scale:8.3f}")
