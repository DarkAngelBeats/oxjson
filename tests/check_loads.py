import os, sys, json, random, math, glob, struct
sys.stdout.reconfigure(errors='backslashreplace')  # docs may hold lone surrogates
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
mod = __import__('oxjson') if len(sys.argv) < 2 or not sys.argv[1] else __import__('oxjson.' + sys.argv[1], fromlist=['x'])
loads = mod.loads
def same(a, b):
    # iterative: testdata/_gen/deep_10000.json is deeper than the interpreter's recursion limits
    st = [(a, b)]
    while st:
        a, b = st.pop()
        if type(a) != type(b): return False
        if isinstance(a, dict):
            if list(a.keys()) != list(b.keys()): return False
            st.extend((a[k], b[k]) for k in a)
        elif isinstance(a, list):
            if len(a) != len(b): return False
            st.extend(zip(a, b))
        elif isinstance(a, float):
            if math.isnan(a):
                if not math.isnan(b): return False
            elif struct.pack('<d', a) != struct.pack('<d', b): return False
        elif a != b: return False
    return True
def norm(o):
    # ints outside -2^63 .. 2^64-1 are floats here (as in orjson); in place, iterative
    big = lambda x: isinstance(x, int) and not isinstance(x, bool) and not (-2**63 <= x < 2**64)
    if big(o): return float(o)
    if not isinstance(o, (list, dict)): return o
    st = [o]
    while st:
        c = st.pop()
        for k, v in (c.items() if isinstance(c, dict) else enumerate(c)):
            if big(v): c[k] = float(v)
            elif isinstance(v, (list, dict)): st.append(v)
    return o
def has_bad(doc):
    # what json accepts and ours rejects (as orjson does): lone surrogates, inf, ints past a double.
    # object_pairs_hook keeps values that a duplicate key would hide; iterative for deep_10000
    st = [json.loads(doc, object_pairs_hook=lambda kv: kv)]
    while st:
        o = st.pop()
        if isinstance(o, float) and math.isinf(o): return True
        if isinstance(o, str) and any(0xD800 <= ord(ch) <= 0xDFFF for ch in o): return True
        if isinstance(o, int) and not isinstance(o, bool) and abs(o) >= 2**1024: return True
        if isinstance(o, (list, tuple)): st.extend(o)
    return False
bad = 0
for f in glob.glob(ROOT + '/testdata/*/*.json'):
    d = open(f, 'rb').read()
    try: ref = json.loads(d)
    except Exception: ref = None
    try: got = loads(d)
    except Exception as e: got = e
    if ref is None:
        continue
    if isinstance(got, Exception):
        # allowed: float overflow, NaN / Infinity and a BOM are errors here (as in orjson)
        s = d.decode('utf-8', 'replace')
        if 'inf' in f or 'nan' in f or 'bom' in f or has_bad(d): continue
        bad += 1; print('ERR on valid', f, got); continue
    if not same(got, norm(ref)): bad += 1; print('MISMATCH', f)
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 7)
ws = ['', ' ', '\n', '\t', '\r\n  ', '    ']
def rnum():
    r = random.random()
    if r < 0.2: return str(random.randint(-10**6, 10**6))
    if r < 0.3: return str(random.randint(-2**63 - 2**20, 2**64 + 2**20))
    if r < 0.5: return repr(random.uniform(-1e6, 1e6))
    if r < 0.6: return '%de%d' % (random.randint(-999, 999), random.randint(-330, 300))
    if r < 0.7: return '%d.%se%+d' % (random.randint(0, 99), ''.join(random.choice('0123456789') for _ in range(random.randint(1, 25))), random.randint(-320, 300))
    if r < 0.8: return '0.' + '0' * random.randint(0, 20) + str(random.randint(1, 10**random.randint(1, 22)))
    if r < 0.9: return ''.join(random.choice('123456789')) + ''.join(random.choice('0123456789') for _ in range(random.randint(0, 30))) + '.' + ''.join(random.choice('0123456789') for _ in range(random.randint(1, 30)))
    return repr(struct.unpack('<d', struct.pack('<Q', random.getrandbits(62)))[0])
chars = ['a', 'b', 'é', '中', '😀', '"', '\\', '\n', '\x01', '/', ' ', 'z' * 33, ' ']
def rstr():
    s = ''.join(random.choice(chars) for _ in range(random.randint(0, 40)))
    out = json.dumps(s, ensure_ascii=random.random() < 0.3)
    return out
def rval(d):
    r = random.random()
    if d > 5 or r < 0.45:
        c = random.randint(0, 5)
        if c == 0: return rstr()
        if c in (1, 2): return rnum()
        if c == 3: return random.choice(['true', 'false', 'null'])
        return rstr()
    w = lambda: random.choice(ws)
    if r < 0.75:
        return '[' + w() + (',' + w()).join(rval(d + 1) for _ in range(random.randint(0, 6))) + w() + ']'
    return '{' + w() + (',' + w()).join(rstr() + w() + ':' + w() + rval(d + 1) for _ in range(random.randint(0, 6))) + w() + '}'
nv = ni = 0
for i in range(40000):
    doc = random.choice(ws) + rval(0) + random.choice(ws)
    try:
        ref = json.loads(doc)
    except Exception:
        continue
    data = doc.encode() if random.random() < 0.5 else doc
    try:
        got = loads(data)
    except Exception as e:
        if has_bad(doc): continue
        bad += 1
        if bad < 10: print('RAISED', repr(doc)[:200], e)
        continue
    nv += 1
    if not same(got, norm(ref)):
        bad += 1
        if bad < 10: print('MISMATCH', repr(doc)[:200])
    # corrupt it
    if len(doc) > 1:
        k = random.randrange(len(doc))
        bad_doc = doc[:k] + random.choice(['', ',', ']', '}', '"', '\\', 'x', '\x00', '{', ':', '1', ' ']) + doc[k + 1:]
        try:
            json.loads(bad_doc); ok_std = True
        except Exception:
            ok_std = False
        try:
            loads(bad_doc); ok_ours = True
        except Exception:
            ok_ours = False
        if ok_std != ok_ours:
            # stdlib allows big ints and inf; ours rejects only overflow
            try:
                if has_bad(bad_doc): continue
            except Exception:
                pass
            bad += 1; ni += 1
            if ni < 10:
                print('VALIDITY', ok_std, ok_ours, repr(bad_doc))
                try: loads(bad_doc)
                except Exception as e: print('   ours:', e)
print('valid docs', nv, 'bad', bad)
sys.exit(1 if bad else 0)
