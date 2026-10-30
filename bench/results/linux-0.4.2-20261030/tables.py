"""Print the report tables from speed-1.json, speed-2.json and memory.json."""
import json
import statistics as st
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIB = 1048576


def name(doc):
    return Path(doc).stem


def speed():
    samples = []
    for f in ("speed-1.json", "speed-2.json"):
        samples += json.loads((HERE / f).read_text())["samples"]
    docs = list(dict.fromkeys(s["document"] for s in samples))
    print("| Документ | Операция | oxjson, мкс | orjson, мкс | oxjson / orjson |")
    print("|---|---|---:|---:|---:|")
    for doc in docs:
        for op in ("loads", "dumps"):
            row = {}
            for lib in ("oxjson", "orjson"):
                v = [s[op + "_us"] for s in samples if s["library"] == lib and s["document"] == doc]
                row[lib] = (st.median(v), min(v), max(v), len(v))
            o, r = row["oxjson"], row["orjson"]
            print(f"| {name(doc)} | {op} | {o[0]:.1f} [{o[1]:.1f}–{o[2]:.1f}] | "
                  f"{r[0]:.1f} [{r[1]:.1f}–{r[2]:.1f}] | {o[0] / r[0]:.3f} |")
    print(f"\nпроцессов на ячейку: {o[3]}")


def memory():
    m = json.loads((HERE / "memory.json").read_text())
    samples = [s for s in m["samples"] if s.get("workload", "document") == "document"]
    docs = list(dict.fromkeys(s["document"] for s in samples))
    print("\n| Документ | Операция | oxjson: один | orjson: один | oxjson: шесть | orjson: шесть |")
    print("|---|---|---:|---:|---:|---:|")
    growth = 0
    for doc in docs:
        for op in ("loads", "dumps"):
            cells = {}
            for lib in ("oxjson", "orjson"):
                ss = [s for s in samples if s["library"] == lib and s["document"] == doc and s["operation"] == op]
                one = [(s["held"]["current_bytes"] - s["baseline"]["current_bytes"]) / MIB for s in ss]
                six = [(s["held_many"]["current_bytes"] - s["released"]["current_bytes"]) / MIB for s in ss]
                for s in ss:
                    growth = max(growth, max(r["current_bytes"] for r in s["rounds"]) - s["warm"]["current_bytes"])
                cells[lib] = (st.median(one), st.median(six))
            print(f"| {name(doc)} | {op} | {cells['oxjson'][0]:.3f} | {cells['orjson'][0]:.3f} | "
                  f"{cells['oxjson'][1]:.3f} | {cells['orjson'][1]:.3f} |")
    print(f"\nстатус: {m['status']}, ошибок: {len(m['errors'])}, "
          f"наибольший рост после прогрева: {growth / MIB:.2f} МиБ")


if __name__ == "__main__":
    speed()
    memory()
