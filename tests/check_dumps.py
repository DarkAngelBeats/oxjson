import os, sys, json, random, glob
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import oxjson as j
def std(o): return json.dumps(o, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
bad = 0
files = [ROOT + '/testdata/bench/large_50k.json', ROOT + '/testdata/engine/atlas.json'] + glob.glob(ROOT + '/testdata/real_small/*.json')
for f in files:
    o = json.load(open(f, encoding='utf-8'))
    if j.dumps(o) != std(o): bad += 1; print('FILE MISMATCH', f)
random.seed(5)
alpha = ['a', 'é', '"', '\\', '\n', '\x00', '\x1f', '\x7f', 'ü', '中', '😀', '/', ' ', 'z' * 40]
def rstr():
    return ''.join(random.choice(alpha) for _ in range(random.randint(0, 70)))
def rval(d):
    r = random.random()
    if d > 4 or r < 0.5:
        c = random.randint(0, 6)
        if c == 0: return rstr()
        if c == 1: return random.randint(-2**63, 2**63 - 1)
        if c == 2: return random.choice([random.random() * 10 ** random.randint(-30, 30), float(random.randint(-10**6, 10**6)), -0.0, 0.0])
        if c == 3: return random.choice([True, False, None])
        if c == 4: return random.randint(-1000, 1000)
        if c == 5: return random.random()
        return rstr()[:3]
    if r < 0.75: return [rval(d + 1) for _ in range(random.randint(0, 8))]
    if r < 0.85: return tuple(rval(d + 1) for _ in range(random.randint(0, 4)))
    return {rstr(): rval(d + 1) for _ in range(random.randint(0, 8))}
for i in range(20000):
    o = rval(0)
    a = j.dumps(o); b = std(o)
    if a != b:
        bad += 1
        if bad < 5: print('MISMATCH', repr(o)[:200], a[:200], b[:200])
# page-end strings: many lengths
for n in range(0, 300):
    for ch in ['a', '"', 'é']:
        s = 'x' * n + ch
        if j.dumps(s) != std(s): bad += 1; print('LEN', n, ch)
print('bad', bad)
