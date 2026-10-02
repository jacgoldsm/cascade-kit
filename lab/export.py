import sys, numpy as np, torch, nnlib
sd = torch.load(sys.argv[1]); out = sys.argv[2]
E = sd['emb.weight'].numpy().reshape(61, 256, -1)[:, :64, :]
H1 = E.shape[2]; H2 = sd['l2.weight'].shape[0]
with open(out, 'wb') as f:
    np.array([H1, H2], np.int32).tofile(f)
    for arr in [E, sd['sc.weight'], sd['sc.bias'], sd['l2.weight'], sd['l2.bias'], sd['l3.weight'], sd['l3.bias'], sd['skip.weight'], sd['skip.bias']]:
        np.ascontiguousarray(arr.numpy() if hasattr(arr, 'numpy') else arr, dtype=np.float32).tofile(f)
print('exported', H1, H2)
