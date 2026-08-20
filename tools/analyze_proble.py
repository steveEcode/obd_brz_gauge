#!/usr/bin/env python3
"""
analyze_probe.py — 把 fake_elm327.py 注入的已知信号，和 Car Scanner 录出来的
传感器数据对上号，反推 "哪个 PID 驱动哪个仪表" 以及 "解码公式是什么"。

关键点：探测日志里每一条注入，都对应 app 的一次真实请求。所以两边的序列
天然一一对应，**按顺序配对即可，不需要对齐时钟**。（Car Scanner 导出的
SECONDS 是设备开机秒数，本来也没法直接和电脑时钟比。）

用法：

  1) 识别
       python3 fake_elm327.py --probe prbs --tick 2
       # Car Scanner: 选车型 profile → 打开所有仪表页 → 开始记录 → 至少 10 分钟
       python3 analyze_probe.py identify --probe probe_log_*.csv --app scanner01.csv

  2) 解公式
       python3 fake_elm327.py --probe basis --dwell 8
       python3 analyze_probe.py formula --probe probe_log_*.csv --app scanner02.csv \
              --map mapping.csv

只依赖标准库。
"""

import argparse
import csv
import math
import os
import statistics
import sys
from collections import defaultdict
from datetime import datetime

BASIS_PATTERNS = 9
MIN_POINTS = 8

# ----------------------------------------------------------------------------
# SAE J1979 标准 mode 01 PID 表。这些不用倒推，公式是公开的。
# 格式: pid -> (名称, 数据字节数, 公式, 单位)
# 字母记号 A/B/C/D = 响应数据段的第 1/2/3/4 字节
# ----------------------------------------------------------------------------

STD_PIDS = {
    0x01: ("Monitor status since DTCs cleared", 4, "(位域)", ""),
    0x02: ("Freeze DTC", 2, "(位域)", ""),
    0x03: ("Fuel system status", 2, "(位域)", ""),
    0x04: ("Calculated engine load", 1, "A*100/255", "%"),
    0x05: ("Engine coolant temperature", 1, "A-40", "degC"),
    0x06: ("Short term fuel trim bank 1", 1, "A/1.28-100", "%"),
    0x07: ("Long term fuel trim bank 1", 1, "A/1.28-100", "%"),
    0x08: ("Short term fuel trim bank 2", 1, "A/1.28-100", "%"),
    0x09: ("Long term fuel trim bank 2", 1, "A/1.28-100", "%"),
    0x0A: ("Fuel pressure", 1, "A*3", "kPa"),
    0x0B: ("Intake manifold absolute pressure", 1, "A", "kPa"),
    0x0C: ("Engine speed", 2, "(A*256+B)/4", "rpm"),
    0x0D: ("Vehicle speed", 1, "A", "km/h"),
    0x0E: ("Timing advance", 1, "A/2-64", "deg"),
    0x0F: ("Intake air temperature", 1, "A-40", "degC"),
    0x10: ("MAF air flow rate", 2, "(A*256+B)/100", "g/s"),
    0x11: ("Throttle position", 1, "A*100/255", "%"),
    0x12: ("Commanded secondary air status", 1, "(位域)", ""),
    0x13: ("Oxygen sensors present (2 banks)", 1, "(位域)", ""),
    0x14: ("O2 sensor 1 voltage / STFT", 2, "A/200", "V"),
    0x15: ("O2 sensor 2 voltage / STFT", 2, "A/200", "V"),
    0x16: ("O2 sensor 3 voltage / STFT", 2, "A/200", "V"),
    0x17: ("O2 sensor 4 voltage / STFT", 2, "A/200", "V"),
    0x18: ("O2 sensor 5 voltage / STFT", 2, "A/200", "V"),
    0x19: ("O2 sensor 6 voltage / STFT", 2, "A/200", "V"),
    0x1A: ("O2 sensor 7 voltage / STFT", 2, "A/200", "V"),
    0x1B: ("O2 sensor 8 voltage / STFT", 2, "A/200", "V"),
    0x1C: ("OBD standards conformance", 1, "(枚举)", ""),
    0x1D: ("Oxygen sensors present (4 banks)", 1, "(位域)", ""),
    0x1E: ("Auxiliary input status", 1, "(位域)", ""),
    0x1F: ("Run time since engine start", 2, "A*256+B", "s"),
    0x21: ("Distance traveled with MIL on", 2, "A*256+B", "km"),
    0x22: ("Fuel rail pressure (rel. to manifold)", 2, "(A*256+B)*0.079", "kPa"),
    0x23: ("Fuel rail gauge pressure", 2, "(A*256+B)*10", "kPa"),
    0x24: ("O2 S1 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x25: ("O2 S2 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x26: ("O2 S3 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x27: ("O2 S4 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x28: ("O2 S5 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x29: ("O2 S6 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x2A: ("O2 S7 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x2B: ("O2 S8 WR lambda equivalence ratio", 4, "(A*256+B)/32768", ""),
    0x2C: ("Commanded EGR", 1, "A*100/255", "%"),
    0x2D: ("EGR error", 1, "A/1.28-100", "%"),
    0x2E: ("Commanded evaporative purge", 1, "A*100/255", "%"),
    0x2F: ("Fuel tank level input", 1, "A*100/255", "%"),
    0x30: ("Warm-ups since codes cleared", 1, "A", ""),
    0x31: ("Distance since codes cleared", 2, "A*256+B", "km"),
    0x32: ("Evap system vapor pressure", 2, "(A*256+B)/4-8192", "Pa"),
    0x33: ("Absolute barometric pressure", 1, "A", "kPa"),
    0x34: ("O2 S1 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x35: ("O2 S2 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x36: ("O2 S3 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x37: ("O2 S4 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x38: ("O2 S5 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x39: ("O2 S6 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x3A: ("O2 S7 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x3B: ("O2 S8 WR lambda current", 4, "(A*256+B)/32768", ""),
    0x3C: ("Catalyst temperature B1S1", 2, "(A*256+B)/10-40", "degC"),
    0x3D: ("Catalyst temperature B2S1", 2, "(A*256+B)/10-40", "degC"),
    0x3E: ("Catalyst temperature B1S2", 2, "(A*256+B)/10-40", "degC"),
    0x3F: ("Catalyst temperature B2S2", 2, "(A*256+B)/10-40", "degC"),
    0x41: ("Monitor status this drive cycle", 4, "(位域)", ""),
    0x42: ("Control module voltage", 2, "(A*256+B)/1000", "V"),
    0x43: ("Absolute load value", 2, "(A*256+B)*100/255", "%"),
    0x44: ("Commanded AFR / lambda", 2, "(A*256+B)/32768", ""),
    0x45: ("Relative throttle position", 1, "A*100/255", "%"),
    0x46: ("Ambient air temperature", 1, "A-40", "degC"),
    0x47: ("Absolute throttle position B", 1, "A*100/255", "%"),
    0x48: ("Absolute throttle position C", 1, "A*100/255", "%"),
    0x49: ("Accelerator pedal position D", 1, "A*100/255", "%"),
    0x4A: ("Accelerator pedal position E", 1, "A*100/255", "%"),
    0x4B: ("Accelerator pedal position F", 1, "A*100/255", "%"),
    0x4C: ("Commanded throttle actuator", 1, "A*100/255", "%"),
    0x4D: ("Time run with MIL on", 2, "A*256+B", "min"),
    0x4E: ("Time since codes cleared", 2, "A*256+B", "min"),
    0x4F: ("Max values (lambda/V/mA/kPa)", 4, "(多值)", ""),
    0x50: ("Max MAF air flow rate", 4, "A*10", "g/s"),
    0x51: ("Fuel type", 1, "(枚举)", ""),
    0x52: ("Ethanol fuel percentage", 1, "A*100/255", "%"),
    0x53: ("Absolute evap system vapor pressure", 2, "(A*256+B)/200", "kPa"),
    0x54: ("Evap system vapor pressure (signed)", 2, "(A*256+B)-32767", "Pa"),
    0x55: ("Short term secondary O2 trim B1/B3", 2, "A/1.28-100", "%"),
    0x56: ("Long term secondary O2 trim B1/B3", 2, "A/1.28-100", "%"),
    0x57: ("Short term secondary O2 trim B2/B4", 2, "A/1.28-100", "%"),
    0x58: ("Long term secondary O2 trim B2/B4", 2, "A/1.28-100", "%"),
    0x59: ("Fuel rail absolute pressure", 2, "(A*256+B)*10", "kPa"),
    0x5A: ("Relative accelerator pedal position", 1, "A*100/255", "%"),
    0x5B: ("Hybrid battery pack remaining life", 1, "A*100/255", "%"),
    0x5C: ("Engine oil temperature", 1, "A-40", "degC"),
    0x5D: ("Fuel injection timing", 2, "(A*256+B)/128-210", "deg"),
    0x5E: ("Engine fuel rate", 2, "(A*256+B)/20", "L/h"),
    0x5F: ("Emission requirements designed to", 1, "(枚举)", ""),
    0x61: ("Driver demand engine torque", 1, "A-125", "%"),
    0x62: ("Actual engine torque", 1, "A-125", "%"),
    0x63: ("Engine reference torque", 2, "A*256+B", "Nm"),
    0x64: ("Engine percent torque data", 5, "A-125", "%"),
    0x66: ("Mass air flow sensor", 5, "(多值)", "g/s"),
    0x67: ("Engine coolant temperature (2 sensors)", 3, "B-40", "degC"),
    0x68: ("Intake air temperature sensor", 7, "(多值)", "degC"),
    0x6B: ("Exhaust gas recirculation temperature", 5, "(多值)", "degC"),
    0x6D: ("Fuel pressure control system", 6, "(多值)", "kPa"),
    0x9D: ("Engine fuel rate (multi)", 4, "(A*256+B)*0.02", "g/s"),
    0x9E: ("Engine exhaust flow rate", 2, "(A*256+B)*0.2", "kg/h"),
    0xA6: ("Odometer", 4, "(A*16777216+B*65536+C*256+D)/10", "km"),
}

MODE09_PIDS = {
    0x00: "Supported PIDs", 0x02: "VIN", 0x04: "Calibration ID",
    0x06: "Calibration verification numbers", 0x08: "In-use performance tracking",
    0x0A: "ECU name", 0x0B: "In-use performance tracking (compression)",
}


def std_info(request):
    """请求 -> (来源, 标准名, 标准公式, 单位)。不是标准 PID 就返回 None。"""
    r = request.strip().upper()
    if len(r) < 4:
        return None
    mode, rest = r[:2], r[2:]
    try:
        pid = int(rest[:2], 16)
    except ValueError:
        return None
    if mode == "01" and len(rest) == 2:
        if pid % 0x20 == 0:
            return ("standard", f"Supported PIDs {pid:02X}-{pid+0x1F:02X}",
                    "(位图)", "")
        if pid in STD_PIDS:
            name, _n, f, u = STD_PIDS[pid]
            return ("standard", name, f, u)
        return ("oem-mode01", f"非标准 mode 01 PID {pid:02X}", "", "")
    if mode == "09" and len(rest) == 2:
        return ("standard", "Vehicle info: " + MODE09_PIDS.get(pid, f"09{pid:02X}"),
                "(文本/枚举)", "")
    if mode in ("03", "04", "07", "0A", "19"):
        return ("standard", "诊断码相关", "(DTC)", "")
    return None


def eval_formula(expr, byts):
    """把 A/B/C/D 记号的公式在给定字节上求值。"""
    if not expr or any(mark in expr for mark in ("位域", "位图", "枚举", "多值", "文本", "DTC")):
        return None
    env = {}
    for i, ch in enumerate("ABCDEFGH"):
        env[ch] = byts[i] if i < len(byts) else 0
    try:
        return float(eval(expr, {"__builtins__": {}}, env))
    except Exception:
        return None

               # 少于这么多配对点就不下结论


# ----------------------------------------------------------------------------
# 读取
# ----------------------------------------------------------------------------

TIME_FMTS = [
    "%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S",
    "%d.%m.%Y %H:%M:%S.%f", "%d.%m.%Y %H:%M:%S",
    "%Y/%m/%d %H:%M:%S.%f", "%Y/%m/%d %H:%M:%S",
    "%m/%d/%Y %H:%M:%S.%f", "%m/%d/%Y %H:%M:%S",
    "%H:%M:%S.%f", "%H:%M:%S",
]


def parse_time(v):
    """返回秒（可能是 epoch，也可能是开机计时，识别阶段不关心绝对值）。"""
    v = v.strip().strip('"')
    if not v:
        return None
    try:
        return float(v)
    except ValueError:
        pass
    for fmt in TIME_FMTS:
        try:
            return datetime.strptime(v, fmt).timestamp()
        except ValueError:
            continue
    return None


def parse_num(v):
    v = v.strip().strip('"')
    if not v:
        return None
    if "," in v and "." not in v:          # 逗号当小数点的地区
        v = v.replace(",", ".")
    try:
        f = float(v)
    except ValueError:
        return None
    return f if math.isfinite(f) else None


def sniff_read(path):
    with open(path, newline="", encoding="utf-8-sig", errors="replace") as f:
        head = f.read(16384)
        f.seek(0)
        try:
            dialect = csv.Sniffer().sniff(head, delimiters=",;\t")
        except csv.Error:
            dialect = csv.excel
            if head.count(";") > head.count(","):
                dialect = csv.excel
                dialect.delimiter = ";"
        return list(csv.reader(f, dialect))


def load_app_csv(path):
    """返回 {传感器名: (时间列表, 数值列表)}，两种排版都支持。"""
    rows = sniff_read(path)
    if len(rows) < 2:
        sys.exit(f"{path} 内容太少")
    header = [h.strip().strip('"') for h in rows[0]]
    up = [h.upper() for h in header]

    if "PID" in up and "VALUE" in up:
        # Car Scanner 的长格式: SECONDS;PID;VALUE;UNITS;LAT;LON
        ti = up.index("SECONDS") if "SECONDS" in up else 0
        pi, vi = up.index("PID"), up.index("VALUE")
        data = defaultdict(lambda: ([], []))
        for r in rows[1:]:
            if len(r) <= max(ti, pi, vi):
                continue
            t = parse_time(r[ti])
            val = parse_num(r[vi])
            name = r[pi].strip().strip('"')
            if t is None or val is None or not name:
                continue
            data[name][0].append(t)
            data[name][1].append(val)
        kind = "长格式"
    else:
        # 宽格式: 一列时间 + 每个传感器一列
        tcol = None
        for i in range(len(header)):
            vals = [r[i] for r in rows[1:21] if i < len(r) and r[i].strip()]
            if vals and sum(parse_time(v) is not None for v in vals) >= len(vals) * 0.8:
                tcol = i
                break
        if tcol is None:
            sys.exit(f"{path} 认不出排版。表头: {header[:8]}")
        data = defaultdict(lambda: ([], []))
        for r in rows[1:]:
            if tcol >= len(r):
                continue
            t = parse_time(r[tcol])
            if t is None:
                continue
            for i, name in enumerate(header):
                if i == tcol or not name or i >= len(r):
                    continue
                val = parse_num(r[i])
                if val is None:
                    continue
                data[name][0].append(t)
                data[name][1].append(val)
        kind = "宽格式"

    out = {}
    for name, (ts, vs) in data.items():
        order = sorted(range(len(ts)), key=lambda i: ts[i])
        out[name] = ([ts[i] for i in order], [vs[i] for i in order])

    counts = sorted(len(v[0]) for v in out.values())
    span = 0.0
    allt = [t for v in out.values() for t in v[0]]
    if allt:
        span = max(allt) - min(allt)
    print(f"  {os.path.basename(path)}: {kind}, {len(out)} 个传感器, "
          f"{sum(counts)} 个样本, 时长 {span:.0f}s")
    if counts:
        med = statistics.median(counts)
        print(f"     每个传感器采样数: 最少 {counts[0]}, 中位 {med:.0f}, 最多 {counts[-1]}")
        if med < MIN_POINTS:
            need = span * MIN_POINTS / max(1e-9, med) / 60.0
            print(f"\n  !! 中位采样数只有 {med:.0f}，低于配对所需的 {MIN_POINTS} 个点。")
            print(f"     按当前速率，要覆盖大多数传感器大约得录 {need:.0f} 分钟。")
            print(f"     或者分批来：每轮只在 app 里启用 20~30 个传感器，轮询就快得多。\n")
    return out


def load_probe_csv(path):
    rows = sniff_read(path)
    header = [h.strip() for h in rows[0]]
    ix = {n: i for i, n in enumerate(header)}
    for n in ["time", "slot", "header", "request", "byte0", "nbytes"]:
        if n not in ix:
            sys.exit(f"{path} 缺列 {n}，这不是 fake_elm327.py 产出的探测日志")
    by_key = defaultdict(list)
    total = 0
    for r in rows[1:]:
        if len(r) < len(header):
            continue
        e = {
            "t": float(r[ix["time"]]),
            "slot": int(r[ix["slot"]]),
            "byte0": int(r[ix["byte0"]]),
            "n": int(r[ix["nbytes"]]),
        }
        by_key[(r[ix["header"]], r[ix["request"]])].append(e)
        total += 1
    for k in by_key:
        by_key[k].sort(key=lambda e: e["t"])
    span = 0.0
    allt = [e["t"] for v in by_key.values() for e in v]
    if allt:
        span = max(allt) - min(allt)
    print(f"  {os.path.basename(path)}: {total} 条注入, {len(by_key)} 个请求, "
          f"时长 {span:.0f}s")
    return by_key


# ----------------------------------------------------------------------------
# 相关性
# ----------------------------------------------------------------------------

def pearson(xs, ys):
    n = len(xs)
    if n < MIN_POINTS:
        return 0.0, n
    mx, my = sum(xs) / n, sum(ys) / n
    vx = sum((x - mx) ** 2 for x in xs)
    vy = sum((y - my) ** 2 for y in ys)
    if vx <= 1e-12 or vy <= 1e-12:
        return 0.0, n
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    return cov / math.sqrt(vx * vy), n



def _bin_train(times, t0, binw, nbins):
    v = [0] * nbins
    for t in times:
        i = int((t - t0) / binw)
        if 0 <= i < nbins:
            v[i] += 1
    return v


def estimate_lag(ptimes, atimes, binw=0.5):
    """估计 app 时间轴相对探测日志的偏移。

    app 每记录一个样本，都对应我们这边的一次注入，所以两个"事件序列"
    整体上就是彼此的平移。做一次互相关取峰值即可 —— 一次算好，全局通用。
    Car Scanner 的 SECONDS 是开机计时，所以先用起点差做基准再搜索。
    """
    if not ptimes or not atimes:
        return 0.0, 0.0
    p0, a0 = min(ptimes), min(atimes)
    pspan = max(ptimes) - p0
    aspan = max(atimes) - a0
    span = max(pspan, aspan, 1.0)
    L0 = a0 - p0

    nb = int(span / binw) + 2
    pb = _bin_train(ptimes, p0, binw, nb)

    def score(L):
        ab = _bin_train([t - L for t in atimes], p0, binw, nb)
        return sum(x * y for x, y in zip(pb, ab))

    best, bestL = -1.0, L0
    d = -span
    while d <= span:
        sc = score(L0 + d)
        if sc > best:
            best, bestL = sc, L0 + d
        d += binw
    # 细化
    fine, fineL = best, bestL
    d = -binw
    while d <= binw:
        sc = score(bestL + d)
        if sc > fine:
            fine, fineL = sc, bestL + d
        d += binw / 20.0
    return fineL, fine


def assign_by_time(events, times, values, L, tol):
    """把每个 app 样本归到它前面最近的一次注入。
    比按序号配对稳 —— app 偶尔漏采一次也不会让后面全部错位。"""
    out = []
    j = 0
    ne = len(events)
    for t, v in zip(times, values):
        target = t - L
        while j + 1 < ne and events[j + 1]["t"] <= target:
            j += 1
        e = events[j]
        if e["t"] <= target and (target - e["t"]) <= tol:
            out.append((e, v))
    return out


def median_gap(events):
    if len(events) < 3:
        return 5.0
    gaps = [events[i + 1]["t"] - events[i]["t"] for i in range(len(events) - 1)]
    return max(0.05, statistics.median(gaps))


def seq_match(inj, vals, max_shift=6):
    """按序列顺序配对，允许小幅错位（丢包/多采一次）。返回 (r, shift, n)。"""
    best = (0.0, 0, 0)
    for sh in range(-max_shift, max_shift + 1):
        xs, ys = [], []
        for i in range(len(vals)):
            j = i + sh
            if 0 <= j < len(inj):
                xs.append(inj[j])
                ys.append(vals[i])
        r, n = pearson(xs, ys)
        if abs(r) > abs(best[0]):
            best = (r, sh, n)
    return best


# ----------------------------------------------------------------------------
# identify
# ----------------------------------------------------------------------------

def cmd_identify(a):
    print("读取输入:")
    by_key = load_probe_csv(a.probe)
    app = load_app_csv(a.app)

    inj_seq = {k: [e["byte0"] for e in v] for k, v in by_key.items()}
    usable = {n: v for n, v in app.items() if len(v[0]) >= MIN_POINTS}
    print(f"\n采样数够用的传感器: {len(usable)} / {len(app)}")
    if not usable:
        sys.exit("没有任何传感器达到最低配对点数，请录久一点或减少启用的传感器")

    # 第一轮：纯序列配对，拿到一批可信匹配
    print("第一轮 — 序列配对...")
    prov = {}
    for name, (ts, vs) in usable.items():
        best = (0.0, None, 0, 0)
        for k, seq in inj_seq.items():
            if not seq or abs(len(seq) - len(vs)) > max(8, 0.6 * max(len(seq), len(vs))):
                continue
            r, sh, n = seq_match(seq, vs, a.max_shift)
            if abs(r) > abs(best[0]):
                best = (r, k, sh, n)
        if best[1] and abs(best[0]) >= a.min_r:
            prov[name] = best

    # 由可信匹配反推时间偏移（比互相关稳，均匀轮询时互相关是平的）
    diffs = []
    for name, (r, k, sh, n) in prov.items():
        ts = usable[name][0]
        ev = by_key[k]
        for i in range(len(ts)):
            j = i + sh
            if 0 <= j < len(ev):
                diffs.append(ts[i] - ev[j]["t"])
    L = statistics.median(diffs) if diffs else 0.0
    print(f"  可信匹配 {len(prov)} 个 -> 时间偏移 L = {L:.3f}s")

    print("第二轮 — 时间归属配对...")
    results = []
    for name, (ts, vs) in usable.items():
        best = (0.0, None, "", 0)
        if name in prov:
            r0, k0, sh0, n0 = prov[name]
            best = (r0, k0, f"seq{sh0:+d}", n0)
        for k, ev in by_key.items():
            if not inj_seq[k]:
                continue
            tol = median_gap(ev) * 2.5
            pairs = assign_by_time(ev, ts, vs, L, tol)
            if len(pairs) >= MIN_POINTS:
                r, n = pearson([e["byte0"] for e, _ in pairs],
                               [v for _, v in pairs])
                if abs(r) > abs(best[0]):
                    best = (r, k, "time", n)
        r, k, how, n = best
        if k and abs(r) >= a.min_r:
            results.append({"sensor": name, "header": k[0], "request": k[1],
                            "r": round(r, 4), "shift": how, "points": n})

    results.sort(key=lambda x: (-abs(x["r"]), x["sensor"]))

    out = a.out or "mapping.csv"
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["sensor", "header", "request", "r",
                                          "shift", "points"])
        w.writeheader()
        w.writerows(results)

    print(f"\n{'='*78}")
    print(f"{'相关':>7} {'点数':>5}  {'header':<10} {'请求':<12} 传感器")
    print(f"{'='*78}")
    for x in results[:80]:
        print(f"{x['r']:>7.3f} {x['points']:>5}  {x['header']:<10} "
              f"{x['request']:<12} {x['sensor'][:40]}")
    if len(results) > 80:
        print(f"   ... 另有 {len(results)-80} 条")

    hit = {(x["header"], x["request"]) for x in results}
    print(f"\n认出 {len(results)} 个传感器（候选 {len(usable)} 个），"
          f"覆盖 {len(hit)} / {len(by_key)} 个请求")
    print(f"结果: {out}")
    if len(results) < len(usable) * 0.6:
        print("\n命中率偏低，通常是这几个原因：")
        print("  · 录得太短 —— 每个传感器至少要 8 次采样，建议 15 分钟以上")
        print("  · 传感器开太多 —— 分批做，每轮 20~30 个，轮询快很多")
        print("  · --tick 太大 —— 相邻两次请求拿到同一个值就没信息量了，试试 --tick 1")


# ----------------------------------------------------------------------------
# formula
# ----------------------------------------------------------------------------



def group_pairs(pairs, drop_boundary=True):
    """按 slot 归组。默认丢掉每段 slot 的第一个样本 —— 图案切换那一瞬间，
    app 显示的到底是新值还是旧值有歧义，留着会污染回归。"""
    g = defaultdict(lambda: ([], []))
    prev = None
    for e, val in pairs:
        sl = e["slot"]
        if (not drop_boundary) or (prev is not None and sl == prev):
            g[sl][0].append(e["byte0"])
            g[sl][1].append(val)
        prev = sl
    return g


def robust_fit(xs, ys):
    """最小二乘 + 一轮离群点剔除。返回 (斜率, 截距, R2, 用了几个点)。"""
    def fit(xs, ys):
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        vx = sum((x - mx) ** 2 for x in xs)
        if vx <= 1e-12:
            return None
        cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        slope = cov / vx
        inter = my - slope * mx
        vy = sum((y - my) ** 2 for y in ys)
        r2 = (cov * cov / (vx * vy)) if vy > 1e-12 else 1.0
        return slope, inter, r2

    if len(xs) < 4:
        return None
    f = fit(xs, ys)
    if f is None:
        return None
    slope, inter, r2 = f
    res = [abs(y - (slope * x + inter)) for x, y in zip(xs, ys)]
    med = statistics.median(res)
    if med > 0:
        keep = [i for i, r in enumerate(res) if r <= 4 * med]
        if 4 <= len(keep) < len(xs):
            f2 = fit([xs[i] for i in keep], [ys[i] for i in keep])
            if f2 and f2[2] > r2:
                return f2[0], f2[1], f2[2], len(keep)
    return slope, inter, r2, len(xs)


def lag_from_mapping(by_key, app, mapping, max_pairs=24):
    """已知 sensor<->key 对应时，用回归拟合优度扫出时间偏移。
    比事件序列互相关可靠得多 —— 后者在均匀轮询下没有峰。"""
    cands = []
    for sensor, key in mapping:
        if sensor in app and key in by_key:
            cands.append((len(app[sensor][0]), sensor, key))
    cands.sort(reverse=True)
    cands = cands[:max_pairs]
    if not cands:
        return 0.0

    ptimes = [e["t"] for v in by_key.values() for e in v]
    atimes = [t for v in app.values() for t in v[0]]
    L0 = min(atimes) - min(ptimes)
    span = max(max(atimes) - min(atimes), max(ptimes) - min(ptimes), 1.0)

    def score(L):
        tot = 0.0
        for _, sensor, key in cands:
            ev = by_key[key]
            ts, vs = app[sensor]
            pairs = assign_by_time(ev, ts, vs, L, median_gap(ev) * 2.5)
            if len(pairs) < MIN_POINTS:
                continue
            g = group_pairs(pairs)
            for slot, (xs, ys) in g.items():
                if slot == 0 or len(xs) < 4:
                    continue
                r, _ = pearson(xs + xs, ys + ys)
                tot += r * r * len(xs)
        return tot

    best, bestL = -1.0, L0
    d = -span
    while d <= span:
        sc = score(L0 + d)
        if sc > best:
            best, bestL = sc, L0 + d
        d += 0.5
    d = -0.5
    while d <= 0.5:
        sc = score(bestL + d)
        if sc > best:
            best, bestL = sc, bestL + d
        d += 0.02
    return bestL


def cmd_formula(a):
    print("读取输入:")
    by_key = load_probe_csv(a.probe)
    app = load_app_csv(a.app)

    mapping = []
    with open(a.map, newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            mapping.append((row["sensor"], (row["header"], row["request"])))
    print(f"  映射表: {len(mapping)} 条")

    LAG = lag_from_mapping(by_key, app, mapping)
    print(f"  时间偏移 L = {LAG:.3f}s")

    letters = "ABCDEFGH"
    rows_out = []
    n_std = 0
    for sensor, key in mapping:
        if sensor not in app or key not in by_key:
            continue

        info = std_info(key[1])
        is_std = bool(info and info[0] == "standard" and info[2])
        if is_std and not a.all:
            # 标准 PID 不需要倒推，公式是公开的
            n_std += 1
            rows_out.append({
                "sensor": sensor, "header": key[0], "request": key[1],
                "source": "standard", "std_name": info[1],
                "formula": info[2], "unit": info[3],
                "offset": "", "coeffs": "", "slots_seen": "", "check": "查表",
            })
            continue
        ev = by_key[key]
        vs = app[sensor][1]
        nbytes = ev[0]["n"]
        slots = [e["slot"] for e in ev]
        injv = [e["byte0"] for e in ev]

        ts = app[sensor][0]
        tol = median_gap(ev) * 2.5
        pairs = assign_by_time(ev, ts, vs, LAG, tol)

        g = defaultdict(lambda: ([], []))
        if len(pairs) >= MIN_POINTS:
            g = group_pairs(pairs)
        else:
            # 时间轴对不上时退回序列配对
            slots = [e["slot"] for e in ev]
            injv = [e["byte0"] for e in ev]
            best_sh, best_score = 0, -1.0
            for sh in range(-a.max_shift, a.max_shift + 1):
                gg = defaultdict(lambda: ([], []))
                for i in range(len(vs)):
                    j = i + sh
                    if 0 <= j < len(slots):
                        gg[slots[j]][0].append(injv[j])
                        gg[slots[j]][1].append(vs[i])
                sc = 0.0
                for slot, (xs, ys) in gg.items():
                    if slot == 0 or len(xs) < 4:
                        continue
                    r, _ = pearson(xs + xs, ys + ys)
                    sc += r * r * len(xs)
                if sc > best_score:
                    best_score, best_sh = sc, sh
            for i in range(len(vs)):
                j = i + best_sh
                if 0 <= j < len(slots):
                    g[slots[j]][0].append(injv[j])
                    g[slots[j]][1].append(vs[i])

        base = statistics.median(g[0][1]) if 0 in g and g[0][1] else None

        coeffs = [None] * nbytes
        r2s = []
        intercepts = []
        for k in range(nbytes):
            xs, ys = g.get(k + 1, ([], []))
            f = robust_fit(xs, ys)
            if f is None:
                continue
            slope, inter, r2, used = f
            if r2 < a.min_r2:
                continue                      # 拟合太差，宁可留空也不给错答案
            coeffs[k] = round(slope, 8)
            intercepts.append(inter)
            if abs(slope) > 1e-9:
                r2s.append(r2)
        if base is None:
            base = statistics.median(intercepts) if intercepts else 0.0

        if not any(c for c in coeffs):
            continue

        # 相邻系数差 256 倍 -> 合并成 (A*256+B)
        terms, k = [], 0
        while k < nbytes:
            c0 = coeffs[k]
            c1 = coeffs[k + 1] if k + 1 < nbytes else None
            if (c0 and c1 and abs(c1) > 1e-12
                    and abs(c0 / c1 - 256.0) < 256 * 0.02):
                terms.append(f"{c1:+.6g}*({letters[k]}*256+{letters[k+1]})")
                k += 2
                continue
            if c0 is not None and abs(c0) >= 1e-9:
                terms.append(f"{c0:+.6g}*{letters[k]}")
            k += 1
        expr = " ".join(terms) if terms else "(无字节影响读数)"
        if abs(base) > 1e-9:
            expr += f" {base:+.6g}"

        worst = min(r2s) if r2s else 0.0
        check = f"线性 R2={worst:.4f} " + ("✓" if worst >= 0.995 else "⚠ 可能非线性")

        # 标准 PID 的话，拿倒推结果和公开公式对一下
        if info and info[0] == "standard" and info[2]:
            std_v = eval_formula(info[2], [0xFF] + [0] * 7)
            der_v = base + (coeffs[0] or 0) * 255
            if std_v is not None:
                tol = max(1e-6, abs(std_v) * 0.03)
                check += "  |  对照标准: " + ("一致 ✓" if abs(std_v - der_v) <= tol
                                            else f"不一致 (标准 {std_v:.4g} vs 倒推 {der_v:.4g})")

        rows_out.append({
            "sensor": sensor, "header": key[0], "request": key[1],
            "source": "standard" if (info and info[0] == "standard") else "derived",
            "std_name": info[1] if info else "",
            "formula": expr.strip(), "unit": info[3] if info else "",
            "offset": base,
            "coeffs": ";".join("" if c is None else f"{c:.8g}" for c in coeffs),
            "slots_seen": len(g), "check": check,
        })

    out = a.out or "formulas.csv"
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["sensor", "header", "request", "source",
                                          "std_name", "formula", "unit", "offset",
                                          "coeffs", "slots_seen", "check"])
        w.writeheader()
        w.writerows(rows_out)

    print(f"\n{'='*104}")
    for x in rows_out:
        tag = "[标准]" if x["source"] == "standard" else "[倒推]"
        print(f"  {tag} {x['header']:<9} {x['request']:<12} {x['sensor'][:30]:<30} "
              f"= {x['formula']:<28} {x['check']}")
    print(f"{'='*104}")
    print(f"\n共 {len(rows_out)} 条（标准查表 {n_std}，倒推 {len(rows_out)-n_std}）-> {out}")
    print("A=响应数据段第 1 字节, B=第 2 字节 …，和 Car Scanner 自定义 PID 的记号一致。")



# ----------------------------------------------------------------------------
# report：汇总成一份完整 PID 清单
# ----------------------------------------------------------------------------

def cmd_report(a):
    """把抓到的所有请求汇成一张表：标准的查表填好，私有的填倒推结果，
    剩下的标为待解 —— 保证每一个见过的请求都在最终清单里。"""
    print("读取输入:")

    # 全量请求清单（fake_elm327.py 的 elm_pids_*.csv）
    requests = []
    with open(a.pids, newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            requests.append((row["header"], row["full_request"],
                             int(row.get("count", 0) or 0)))
    print(f"  {os.path.basename(a.pids)}: {len(requests)} 个请求")

    # 传感器映射（可选）
    sensors_of = defaultdict(list)
    if a.map and os.path.exists(a.map):
        with open(a.map, newline="", encoding="utf-8-sig") as f:
            for row in csv.DictReader(f):
                sensors_of[(row["header"], row["request"])].append(
                    (row["sensor"], row.get("r", "")))
        print(f"  {os.path.basename(a.map)}: {sum(len(v) for v in sensors_of.values())} 条映射")

    # 倒推公式（可选）
    formula_of = {}
    if a.formulas and os.path.exists(a.formulas):
        with open(a.formulas, newline="", encoding="utf-8-sig") as f:
            for row in csv.DictReader(f):
                formula_of[(row["header"], row["request"], row["sensor"])] = row
        print(f"  {os.path.basename(a.formulas)}: {len(formula_of)} 条公式")

    out_rows = []
    stats = defaultdict(int)
    for hdr, req, cnt in requests:
        info = std_info(req)
        mode, pid = req[:2], req[2:]
        base = {"header": hdr, "mode": mode, "pid": pid, "request": req,
                "count": cnt}

        names = sensors_of.get((hdr, req)) or [("", "")]
        for sensor, r in names:
            row = dict(base)
            row["sensor"] = sensor
            row["r"] = r

            fk = (hdr, req, sensor)
            derived = formula_of.get(fk)

            if info and info[0] == "standard":
                row["source"] = "standard"
                row["name"] = info[1]
                row["formula"] = info[2]
                row["unit"] = info[3]
                row["note"] = "SAE J1979 查表"
                stats["standard"] += 1
            elif derived and derived.get("formula") and \
                    not derived["formula"].startswith("(无"):
                row["source"] = "derived"
                row["name"] = sensor
                row["formula"] = derived["formula"]
                row["unit"] = derived.get("unit", "")
                row["note"] = derived.get("check", "")
                stats["derived"] += 1
            elif info and info[0] == "oem-mode01":
                row["source"] = "oem-mode01"
                row["name"] = sensor or info[1]
                row["formula"] = ""
                row["unit"] = ""
                row["note"] = "厂家扩展的 mode 01 PID，需倒推"
                stats["待解"] += 1
            else:
                row["source"] = "proprietary"
                row["name"] = sensor
                row["formula"] = ""
                row["unit"] = ""
                row["note"] = "私有 PID" + ("" if sensor else "，且未对上任何仪表")
                stats["待解"] += 1
            out_rows.append(row)

    def sortkey(x):
        rank = {"standard": 0, "derived": 1, "oem-mode01": 2, "proprietary": 3}
        return (rank.get(x["source"], 9), x["header"], x["request"])
    out_rows.sort(key=sortkey)

    out = a.out or "pids_full.csv"
    fields = ["source", "header", "mode", "pid", "request", "name", "sensor",
              "formula", "unit", "r", "count", "note"]
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(out_rows)

    print(f"\n{'='*104}")
    cur = None
    for x in out_rows:
        if x["source"] != cur:
            cur = x["source"]
            label = {"standard": "标准 PID（查表，无需倒推）",
                     "derived": "私有 PID（已倒推出公式）",
                     "oem-mode01": "厂家扩展 mode 01（待解）",
                     "proprietary": "私有 PID（待解）"}.get(cur, cur)
            print(f"\n--- {label} ---")
        nm = x["name"] or x["sensor"] or "?"
        print(f"  {x['header']:<9} {x['request']:<12} {nm[:36]:<36} "
              f"{x['formula'][:26]:<26} {x['unit']}")
    print(f"\n{'='*104}")
    for k in ("standard", "derived", "待解"):
        if stats[k]:
            print(f"  {k:<10} {stats[k]}")
    print(f"\n完整清单 ({len(out_rows)} 行): {out}")
    if stats["待解"]:
        print(f"\n还有 {stats['待解']} 条没解出来。跑一轮 --probe basis 再执行 "
              f"formula，然后把 --formulas 传进来重新 report。")


# ----------------------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description="把注入信号和 Car Scanner 录制数据对上号")
    sub = p.add_subparsers(dest="cmd", required=True)

    i = sub.add_parser("identify", help="认出哪个请求驱动哪个仪表（配合 --probe prbs）")
    i.add_argument("--probe", required=True, help="fake_elm327.py 产出的 probe_log_*.csv")
    i.add_argument("--app", required=True, help="Car Scanner 导出的记录 CSV")
    i.add_argument("--out", help="输出映射表，默认 mapping.csv")
    i.add_argument("--min-r", type=float, default=0.9,
                   help="相关性阈值。序列配对该很干净，默认卡到 0.9")
    i.add_argument("--max-shift", type=int, default=6, help="允许的序列错位格数")
    i.set_defaults(func=cmd_identify)

    f = sub.add_parser("formula", help="解出每个字节的系数（配合 --probe basis）")
    f.add_argument("--probe", required=True)
    f.add_argument("--app", required=True)
    f.add_argument("--map", required=True, help="identify 产出的 mapping.csv")
    f.add_argument("--out", help="输出公式表，默认 formulas.csv")
    f.add_argument("--max-shift", type=int, default=6)
    f.add_argument("--min-r2", type=float, default=0.98,
                   help="单个字节的回归 R2 低于此值就不采信这一项")
    f.add_argument("--all", action="store_true",
                   help="标准 PID 也倒推一遍，用来交叉验证整条流水线是否可信")
    f.set_defaults(func=cmd_formula)

    rp = sub.add_parser("report", help="汇总成一份完整的 PID 清单")
    rp.add_argument("--pids", required=True,
                    help="fake_elm327.py 产出的 elm_pids_*.csv（全量请求清单）")
    rp.add_argument("--map", help="identify 产出的 mapping.csv（可选）")
    rp.add_argument("--formulas", help="formula 产出的 formulas.csv（可选）")
    rp.add_argument("--out", help="输出，默认 pids_full.csv")
    rp.set_defaults(func=cmd_report)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()