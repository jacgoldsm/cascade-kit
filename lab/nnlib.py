import numpy as np, torch, torch.nn as nn
N = 61
REC = np.dtype([('c', np.uint8, 61), ('stm', np.uint8), ('ply', np.uint8), ('cap0', np.uint8), ('cap1', np.uint8),
                ('score', np.int16), ('final', np.int8), ('pad', np.uint8)])
# geometry
cells = []
for r in range(-4, 5):
    for q in range(max(-4, -4 - r), min(4, 4 - r) + 1):
        cells.append((q, r))
index = {c: i for i, c in enumerate(cells)}
def sym_perms():
    perms = []
    for refl in range(2):
        for rot in range(6):
            p = []
            for (q, r) in cells:
                s = -q - r
                x, y, z = q, r, s
                if refl: x, y, z = x, z, y
                for _ in range(rot): x, y, z = -y, -z, -x
                p.append(index[(x, y)])
            perms.append(p)
    return np.array(perms, dtype=np.int64)  # perms[k][i] = image of cell i
PERMS = sym_perms()
# code tables
H = np.zeros(256, np.int64); FLIP = np.zeros(256, np.int64); TOPC = np.zeros(256, np.int64); CNTB = np.zeros(256, np.int64)
for code in range(1, 256):
    h = code.bit_length() - 1
    H[code] = h; bits = code & ((1 << h) - 1)
    FLIP[code] = (1 << h) | (bits ^ ((1 << h) - 1))
    TOPC[code] = -1 if h == 0 else (bits >> (h - 1)) & 1
    CNTB[code] = bin(bits).count('1')

def load(files):
    return np.concatenate([np.fromfile(f, dtype=REC) for f in files])

def prepare(recs):
    """stm-relative codes [n,61], scalars [n,k], target win (stm) and margin."""
    c = recs['c'].astype(np.int64)
    stm = recs['stm'].astype(np.int64)
    c = np.where(stm[:, None] == 1, FLIP[c], c)  # now 'white' bits = stm's pieces
    cap_me = np.where(stm == 0, recs['cap0'], recs['cap1']).astype(np.float32)
    cap_op = np.where(stm == 0, recs['cap1'], recs['cap0']).astype(np.float32)
    top = TOPC[c]
    stk_me = (top == 0).sum(1).astype(np.float32); stk_op = (top == 1).sum(1).astype(np.float32)
    komi = np.where(stm == 0, -0.5, 0.5).astype(np.float32)
    margin = stk_me + cap_me - stk_op - cap_op + komi
    ply = recs['ply'].astype(np.float32) / 150.0
    scal = np.stack([margin / 10, (cap_me - cap_op) / 10, ply, ply * margin / 10], 1).astype(np.float32)
    fm = np.where(stm == 0, recs['final'], -recs['final'].astype(np.int64)).astype(np.float32) / 2
    win = (fm > 0).astype(np.float32)
    return c.astype(np.uint8), scal, win, fm, recs['score'].astype(np.float32)

NSCAL = 4
class Net(nn.Module):
    def __init__(self, H1=64, H2=32):
        super().__init__()
        self.emb = nn.EmbeddingBag(N * 256, H1, mode='sum')
        nn.init.normal_(self.emb.weight, 0, 0.01)
        self.sc = nn.Linear(NSCAL, H1)
        self.l2 = nn.Linear(H1, H2)
        self.l3 = nn.Linear(H2, 1)
        self.skip = nn.Linear(NSCAL, 1)
    def forward(self, codes, scal):
        idx = codes.long() + torch.arange(N, device=codes.device)[None, :] * 256
        a = self.emb(idx) + self.sc(scal)
        a = torch.clamp(a, 0, 1)
        b = torch.clamp(self.l2(a), 0, 1)
        return self.l3(b).squeeze(1) + self.skip(scal).squeeze(1)
