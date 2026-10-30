# A/B of dumps: fresh process per sample, base and cur alternate. usage: ab.py ROOT PAIRS DOC...
import subprocess, sys, statistics as st, json, os
R, pairs, A, Bn, docs = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5:]
W = r'''
import sys, time, gc
sys.path.insert(0, ".")
import oxjson
obj = oxjson.loads(open(sys.argv[1], "rb").read())
for _ in range(10): oxjson.dumps(obj)
ts = []
for _ in range(60):
    t = time.perf_counter_ns(); oxjson.dumps(obj); ts.append(time.perf_counter_ns() - t)
ts.sort(); print(ts[len(ts)//2] / 1000)
'''
res = {}
for d in docs:
    r = {A: [], Bn: []}
    for i in range(pairs):
        for t in ((A, Bn) if i % 2 == 0 else (Bn, A)):
            p = subprocess.run([sys.executable, "-c", W, d], cwd=os.path.join(R, t), capture_output=True, text=True, check=True)
            r[t].append(float(p.stdout))
    b, c = st.median(r[A]), st.median(r[Bn])
    wins = sum(1 for x, y in zip(r[A], r[Bn]) if y < x)
    print("%-15s base %9.2f [%9.2f-%9.2f]  cur %9.2f [%9.2f-%9.2f]  cur/base %.3f  pairs where cur faster %d/%d" % (
        os.path.basename(d), b, min(r[A]), max(r[A]), c, min(r[Bn]), max(r[Bn]), c / b, wins, pairs))
    res[d] = r
json.dump(res, open(os.path.join(R, "out", "ab-%s-%s-%s.json" % (sys.platform, A, Bn)), "w"), indent=1)
