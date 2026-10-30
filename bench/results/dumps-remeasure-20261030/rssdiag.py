import ctypes, sys
sys.path.insert(0, ".")
lib, doc = sys.argv[1], sys.argv[2]
import orjson
L = __import__(lib)
libc = ctypes.CDLL("libc.so.6")
rss = lambda: int(open("/proc/self/statm").read().split()[1]) * 4096 / 1048576
obj = orjson.loads(open(doc, "rb").read())
b = rss(); out = L.dumps(obj); h = rss(); libc.malloc_trim(0); t = rss()
print("%-7s %-15s out %.2f MiB  held-base %.3f  after malloc_trim %.3f" % (lib, doc.split("/")[-1], len(out) / 1048576, h - b, t - b))
