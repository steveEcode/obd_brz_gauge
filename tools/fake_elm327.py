#!/usr/bin/env python3
"""
fake_elm327.py — 假装自己是 WiFi ELM327 的 TCP 服务器。

用途：让 Car Scanner / Torque 等 app 连上来，把它发出的每一条请求
（AT 命令、ECU header、mode 01/09/21/22 的 PID）全部记录下来。

核心思路：对任何未知请求都返回"肯定响应"而不是 NO DATA，
这样 app 不会把 PID 标记为不支持，会继续把 profile 里的全套私有 PID 都问一遍。

用法:
    python3 fake_elm327.py                          # 监听 0.0.0.0:35000
    python3 fake_elm327.py --vin WVWZZZAUZJW123456  # 指定 VIN 触发车型自动识别
    python3 fake_elm327.py --port 35000 -v          # 实时打印每一条

然后手机 Car Scanner:
    Settings → Connection → WiFi → IP = 本机局域网 IP, Port = 35000
    连上后手动选一个车型 profile（大众/宝马/丰田/斯巴鲁都行），
    再打开所有仪表页 + 跑一次 full scan，它就会把该 profile 的 PID 全发一遍。

Ctrl-C 退出时会打印去重汇总，并写出 CSV。
"""

import argparse
import csv
import math
import os
import random
from random import Random
import socket
import sys
import threading
import time
from collections import OrderedDict
from datetime import datetime

# ----------------------------------------------------------------------------
# 全局记录
# ----------------------------------------------------------------------------

LOG_LOCK = threading.Lock()
LOG_ROWS = []                      # 全量流水
SEEN = OrderedDict()               # (header, request) -> 次数
START = time.time()
VERBOSE = False


def record(header, req, resp, kind):
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    with LOG_LOCK:
        LOG_ROWS.append({
            "time": ts,
            "header": header,
            "request": req,
            "kind": kind,
            "response": resp.replace("\r", " | ").strip(),
        })
        key = (header, req)
        SEEN[key] = SEEN.get(key, 0) + 1
        first_time = SEEN[key] == 1
    if VERBOSE or (first_time and kind != "AT"):
        star = " *NEW*" if first_time and kind != "AT" else ""
        print(f"[{ts}] {header:>8} <- {req:<12} -> {resp.strip()[:60]}{star}")


# ----------------------------------------------------------------------------
# 数据生成：给未知 PID 编造一个会缓慢变化的合理数值
# ----------------------------------------------------------------------------

# 常见 mode 01 PID 的数据字节数，让基础仪表看起来正常
MODE01_LEN = {
    0x03: 2, 0x04: 1, 0x05: 1, 0x06: 1, 0x07: 1, 0x08: 1, 0x09: 1,
    0x0A: 1, 0x0B: 1, 0x0C: 2, 0x0D: 1, 0x0E: 1, 0x0F: 1, 0x10: 2,
    0x11: 1, 0x14: 2, 0x15: 2, 0x1F: 2, 0x21: 2, 0x22: 2, 0x23: 2,
    0x2C: 1, 0x2D: 1, 0x2E: 1, 0x2F: 1, 0x30: 1, 0x31: 2, 0x33: 1,
    0x42: 2, 0x43: 2, 0x44: 2, 0x45: 1, 0x46: 1, 0x47: 1, 0x48: 1,
    0x49: 1, 0x4A: 1, 0x4B: 1, 0x4C: 1, 0x4D: 2, 0x4E: 2, 0x5A: 1,
    0x5C: 1, 0x5E: 2,
}


# ----------------------------------------------------------------------------
# 探测引擎：主动往响应里注入已知信号，好把 PID 和 app 上的仪表对上号
#
#   prbs  —— 每个请求分到一条互不相关的伪随机序列，所有请求同时进行。
#            事后把 app 录的 CSV 和这里的日志做互相关，一次性认全部。
#   basis —— 全零 / 逐字节置 FF / 全 FF 轮流来。
#            对线性公式（绝大多数）可以直接解出每个字节的系数和偏移。
# ----------------------------------------------------------------------------

PROBE_IDX = {}            # (header, request) -> 稳定编号
PROBE_LOG = []            # 注入记录，退出时写 CSV
PROBE_LOCK = threading.Lock()

BASIS_PATTERNS = 9        # 0=全零基线, 1..8=只让第 k 个字节取随机值


def probe_bytes(cfg, header, req, n, seed=0):
    """返回要注入的 n 个字节，并记录下来。probe=off 时退化为原来的假数据。"""
    if cfg.probe == "off":
        return fake_bytes(n, seed)

    n = max(1, min(n, 8))
    key = (header, req)
    with PROBE_LOCK:
        idx = PROBE_IDX.setdefault(key, len(PROBE_IDX))

    now = time.time()
    el = now - START

    if cfg.probe == "prbs":
        tick = int(el / cfg.tick)
        # 每个请求一条独立序列：种子里同时混入请求编号和 tick
        rnd = Random((idx * 2654435761) ^ (tick * 40503) ^ 0x5BF03635)
        v = rnd.randrange(0x08, 0xF8)
        out = [v] * n
        slot = tick
    else:  # basis
        # 只让一个字节动，而且每次注入都换一个新的随机值。
        # 这样每个字节的系数可以用回归解出来（比两点求差准得多），
        # 而且随机值让序列对齐不再有歧义。
        p = int(el / cfg.dwell) % BASIS_PATTERNS
        k = p - 1
        out = [0x00] * n
        v = 0
        if 0 <= k < n:
            rnd = Random((idx * 2654435761) ^ (len(PROBE_LOG) * 2246822519)
                         ^ int(now * 1000))
            v = rnd.randrange(1, 256)
            out[k] = v
        else:
            p = 0                    # 该请求没这么多字节，退化成基线
        slot = p

    with PROBE_LOCK:
        PROBE_LOG.append({
            "time": f"{now:.3f}",
            "iso": datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M:%S.%f")[:-3],
            "slot": slot,
            "idx": idx,
            "header": header,
            "request": req,
            "nbytes": n,
            "hex": "".join("%02X" % b for b in out),
            "value": int.from_bytes(bytes(out[:2] if n >= 2 else out), "big"),
            "byte0": (out[0] if cfg.probe == "prbs" else v),
        })
    return out


def fake_bytes(n, seed=0):
    """生成 n 个随时间缓慢变化的字节，让 app 的图表动起来。"""
    t = time.time() - START
    out = []
    for i in range(n):
        v = 0.5 + 0.45 * math.sin(t / 7.0 + seed * 0.37 + i * 1.1)
        out.append(int(v * 255) & 0xFF)
    return out


# ----------------------------------------------------------------------------
# ELM327 输出格式化
# ----------------------------------------------------------------------------

def hexjoin(byts, spaces):
    s = " " if spaces else ""
    return s.join("%02X" % b for b in byts)


def resp_header_for(req_hdr):
    """请求 header -> 响应 header。7DF/7E0->7E8, 7E1->7E9 ... 29bit 也处理。"""
    try:
        h = int(req_hdr, 16)
    except ValueError:
        return "7E8"
    if req_hdr.upper() in ("7DF", ""):
        return "7E8"
    if len(req_hdr) <= 3:                 # 11-bit
        if 0x7E0 <= h <= 0x7E7:
            return "%03X" % (h + 8)
        return "%03X" % (h + 8 & 0x7FF)
    # 29-bit: 18DB33F1 / 18DAxxF1 -> 18DAF1xx
    if len(req_hdr) == 8:
        tgt = (h >> 8) & 0xFF
        if tgt == 0x33:
            tgt = 0x10
        return "18DAF1%02X" % tgt
    return req_hdr


def format_response(payload, req_hdr, headers_on, spaces_on):
    """把 payload（不含 PCI）按 ELM327 的样子排版成一行或多行。"""
    rh = resp_header_for(req_hdr)
    lines = []
    n = len(payload)

    if n <= 7:
        if headers_on:
            lines.append(hexjoin([n] + payload, spaces_on)
                         if not spaces_on else rh + " " + hexjoin([n] + payload, True))
            if not spaces_on:
                lines[0] = rh + hexjoin([n] + payload, False)
        else:
            lines.append(hexjoin(payload, spaces_on))
    else:
        if headers_on:
            first = [0x10 | ((n >> 8) & 0x0F), n & 0xFF] + payload[:6]
            sep = " " if spaces_on else ""
            lines.append(rh + sep + hexjoin(first, spaces_on))
            idx, sn = 6, 1
            while idx < n:
                chunk = payload[idx:idx + 7]
                lines.append(rh + sep + hexjoin([0x20 | (sn & 0x0F)] + chunk, spaces_on))
                idx += 7
                sn = (sn + 1) & 0x0F
        else:
            lines.append("%03X" % n)
            lines.append("0:" + (" " if spaces_on else "") + hexjoin(payload[:6], spaces_on))
            idx, ln = 6, 1
            while idx < n:
                chunk = payload[idx:idx + 7]
                lines.append("%X:" % ln + (" " if spaces_on else "") + hexjoin(chunk, spaces_on))
                idx += 7
                ln = (ln + 1) & 0xF
    return "\r".join(lines) + "\r"


# ----------------------------------------------------------------------------
# 会话状态 + 命令处理
# ----------------------------------------------------------------------------

class Session:
    def __init__(self, cfg):
        self.cfg = cfg
        self.echo = True
        self.headers = False
        self.spaces = True
        self.linefeed = False
        self.header = "7DF"
        self.proto = "6"
        self.reset()

    def reset(self):
        self.echo = True
        self.headers = False
        self.spaces = True
        self.header = "7DF"


def handle_at(sess, cmd):
    """返回 (响应文本, 是否已处理)。cmd 已去掉 AT 前缀并大写。"""
    c = cmd
    if c.startswith("Z") or c.startswith("WS") or c == "D":
        sess.reset()
        return "ELM327 v1.5\r", True
    if c == "I":
        return "ELM327 v1.5\r", True
    if c.startswith("@1"):
        return "OBDII to RS232 Interpreter\r", True
    if c.startswith("@2") or c.startswith("@3"):
        return "?\r", True
    if c.startswith("RV"):
        return "13.8V\r", True
    if c == "DPN":
        return sess.proto + "\r", True
    if c == "DP":
        return "ISO 15765-4 (CAN 11/500)\r", True
    if c.startswith("E"):
        sess.echo = c.endswith("1")
        return "OK\r", True
    if c.startswith("H"):
        sess.headers = c.endswith("1")
        return "OK\r", True
    if c.startswith("S") and len(c) == 2 and c[1] in "01":
        sess.spaces = c.endswith("1")
        return "OK\r", True
    if c.startswith("L"):
        sess.linefeed = c.endswith("1")
        return "OK\r", True
    if c.startswith("SH"):
        sess.header = c[2:].strip()
        return "OK\r", True
    if c.startswith("SP"):
        p = c[2:].strip().lstrip("A")
        if p and p != "0":
            sess.proto = p
        return "OK\r", True
    if c.startswith("TP"):
        return "OK\r", True
    # 其余 AT 命令一律 OK（ATAL/ATST/ATAT/ATCAF/ATCRA/ATFC/ATCM/ATCF/ATPB/ATBRD...）
    return "OK\r", True


def build_obd_response(sess, req, cfg):
    """针对一条 OBD 请求，构造 payload（十进制字节列表）；None = NO DATA。"""
    try:
        data = bytes.fromhex(req)
    except ValueError:
        return None
    if not data:
        return None

    mode = data[0]

    # ---- mode 01：当前数据 ----
    if mode == 0x01 and len(data) >= 2:
        pid = data[1]
        if pid % 0x20 == 0:                       # 支持位图
            if pid >= 0xC0:
                return [0x41, pid, 0xFF, 0xFF, 0xFF, 0xFE]
            return [0x41, pid, 0xFF, 0xFF, 0xFF, 0xFF]
        n = MODE01_LEN.get(pid, cfg.default_len)
        return [0x41, pid] + probe_bytes(cfg, sess.header, req, n, pid)

    # ---- mode 09：车辆信息 ----
    if mode == 0x09 and len(data) >= 2:
        pid = data[1]
        if pid == 0x00:
            return [0x49, 0x00, 0x55, 0x40, 0x00, 0x00]
        if pid == 0x02:
            return [0x49, 0x02, 0x01] + list(cfg.vin.encode())
        if pid == 0x04:
            return [0x49, 0x04, 0x01] + list(cfg.calid.encode())
        if pid == 0x06:
            return [0x49, 0x06, 0x01, 0x12, 0x34, 0x56, 0x78]
        if pid == 0x0A:
            return [0x49, 0x0A, 0x01] + list(b"ECM-EngineControl")
        return [0x49, pid, 0x01] + fake_bytes(4, pid)

    # ---- mode 03/07/0A：DTC ----
    if mode in (0x03, 0x07, 0x0A):
        return [mode + 0x40, 0x00]

    # ---- mode 04：清码 ----
    if mode == 0x04:
        return [0x44]

    # ---- mode 22 (UDS ReadDataByIdentifier)：厂家私有的主战场 ----
    if mode == 0x22 and len(data) >= 3:
        did = list(data[1:3])
        # F190 = VIN (UDS)
        if did == [0xF1, 0x90]:
            return [0x62, 0xF1, 0x90] + list(cfg.vin.encode())
        if did == [0xF1, 0x8C]:
            return [0x62, 0xF1, 0x8C] + list(b"SN0000000001")
        if did == [0xF1, 0x87]:
            return [0x62, 0xF1, 0x87] + list(b"PN000000")
        return [0x62] + did + probe_bytes(cfg, sess.header, req, cfg.default_len, did[1])

    # ---- mode 21 (丰田/斯巴鲁常用的 KWP ReadDataByLocalId) ----
    if mode == 0x21 and len(data) >= 2:
        return [0x61, data[1]] + probe_bytes(cfg, sess.header, req, cfg.default_len, data[1])

    # ---- mode 10 (会话控制) / 3E (TesterPresent) / 27 (SecurityAccess) ----
    if mode == 0x10 and len(data) >= 2:
        return [0x50, data[1], 0x00, 0x32, 0x01, 0xF4]
    if mode == 0x3E:
        return [0x7E, 0x00]
    if mode == 0x27 and len(data) >= 2:
        if data[1] % 2 == 1:                       # requestSeed
            return [0x67, data[1], 0x11, 0x22, 0x33, 0x44]
        return [0x67, data[1]]
    if mode == 0x19:                               # UDS 读 DTC
        return [0x59, data[1] if len(data) > 1 else 0x02, 0xFF]
    if mode == 0x2F and len(data) >= 3:            # IO 控制
        return [0x6F] + list(data[1:4])
    if mode == 0x31 and len(data) >= 4:            # routine control
        return [0x71] + list(data[1:4])

    # ---- 兜底：万能肯定响应 ----
    if cfg.nodata_unknown:
        return None
    return [mode + 0x40] + list(data[1:3]) + probe_bytes(cfg, sess.header, req, cfg.default_len, mode)


# ----------------------------------------------------------------------------
# TCP 服务 —— 故意用最朴素的 accept 循环，不走 socketserver。
# socketserver 的 selector + handle_error 会把"连接明明握手了却没人 accept"
# 这类问题藏起来；这里每 accept 一条就立刻打日志，异常全部原样打出来。
# ----------------------------------------------------------------------------

CONN_SEQ = 0
CONN_LOCK = threading.Lock()


class Conn:
    def __init__(self, sock, addr, cfg, cid):
        self.sock = sock
        self.addr = "%s:%d" % addr
        self.cfg = cfg
        self.cid = cid
        self.sess = Session(cfg)
        self.nbytes = 0
        self.t0 = time.time()

    def send(self, data):
        try:
            self.sock.sendall(data)
            return True
        except OSError as e:
            print(f"    #{self.cid} 发送失败: {type(e).__name__} errno={e.errno} {e}")
            return False

    def run(self):
        cfg = self.cfg
        try:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        self.sock.settimeout(cfg.idle_timeout)

        greet_ok = None
        if cfg.greet == "prompt":
            greet_ok = self.send(b"\r>")
        elif cfg.greet == "banner":
            greet_ok = self.send(b"\r\rELM327 v1.5\r\r>")

        buf = b""
        try:
            while True:
                try:
                    chunk = self.sock.recv(4096)
                except socket.timeout:
                    print(f"    #{self.cid} 空闲超时")
                    break
                except OSError as e:
                    print(f"    #{self.cid} 读失败: {type(e).__name__} errno={e.errno}")
                    break
                if not chunk:
                    break
                if self.nbytes == 0:
                    print(f"\n>>> #{self.cid} {self.addr} 开始发数据\n")
                self.nbytes += len(chunk)
                if cfg.raw:
                    print(f"    #{self.cid} RAW <- {chunk!r}")
                buf += chunk
                buf = buf.replace(b"\r\n", b"\r").replace(b"\n", b"\r")
                while b"\r" in buf:
                    line, buf = buf.split(b"\r", 1)
                    cmd = line.decode("ascii", "ignore").strip()
                    if not cmd:
                        self.send(b"\r>")
                        continue
                    out = self.process(cmd)
                    if self.sess.echo:
                        out = cmd + "\r" + out
                    if self.sess.linefeed:
                        out = out.replace("\r", "\r\n")
                    if not self.send(out.encode() + b"\r>"):
                        return
        except Exception:
            import traceback
            print(f"    #{self.cid} 处理线程异常:")
            traceback.print_exc()
        finally:
            self.finish(greet_ok)
            try:
                self.sock.close()
            except OSError:
                pass

    def finish(self, greet_ok):
        dur = time.time() - self.t0
        if self.nbytes:
            print(f"\n<<< #{self.cid} {self.addr} 断开（{dur:.1f}s，收到 {self.nbytes} 字节）\n")
        elif greet_ok is False:
            print(f"    #{self.cid} [{dur*1000:.0f}ms] 招呼都没发出去，连接已被对端 RST")
        elif dur < 0.5:
            print(f"    #{self.cid} [{dur*1000:.0f}ms] 端口探测，连上即断（正常）")
        else:
            print(f"    #{self.cid} [{dur:.1f}s] 连上但一字节未发，"
                  f"试试换 --greet none 或 --greet banner")

    def process(self, cmd):
        cfg = self.cfg
        sess = self.sess
        raw = cmd.replace(" ", "").upper()

        if raw.startswith("AT"):
            resp, _ = handle_at(sess, raw[2:])
            record("-", cmd.upper(), resp, "AT")
            return resp

        if raw.startswith("ST"):          # STN/OBDLink 专有命令：装作不认识
            record("-", cmd.upper(), "?", "ST")
            return "?\r"

        # 末尾可能带响应帧数提示，例如 "0100 1"
        parts = cmd.split()
        if len(parts) > 1 and len(parts[-1]) == 1 and parts[-1].isdigit():
            req = "".join(parts[:-1]).upper()
        else:
            req = raw

        payload = build_obd_response(sess, req, cfg)
        if payload is None:
            resp = "NO DATA\r"
        else:
            resp = format_response(payload, sess.header, sess.headers, sess.spaces)
        record(sess.header, req, resp, "OBD")
        time.sleep(cfg.delay)
        return resp


def serve(cfg):
    """最朴素的 accept 循环。每接到一条连接立刻打印，不给问题藏身之地。"""
    global CONN_SEQ
    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # 明确不开 SO_REUSEADDR / SO_REUSEPORT：
    # macOS 上开了会允许多个进程同时监听同一端口，内核随机分流，
    # 表现就是 tcpdump 看得到连接、本进程却收不到。
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 0)
    ls.bind((cfg.host, cfg.port))
    ls.listen(128)
    ls.settimeout(1.0)
    print(f"  listen fd={ls.fileno()} backlog=128 就绪，等待连接...\n")
    while True:
        try:
            sock, addr = ls.accept()
        except socket.timeout:
            continue
        except OSError as e:
            print(f"  accept 失败: {e}")
            continue
        with CONN_LOCK:
            CONN_SEQ += 1
            cid = CONN_SEQ
        print(f"    #{cid} accept {addr[0]}:{addr[1]}")
        threading.Thread(target=Conn(sock, addr, cfg, cid).run, daemon=True).start()




# ----------------------------------------------------------------------------

def dump(outdir):
    if not LOG_ROWS:
        print("没有记录到任何请求。")
        return

    os.makedirs(outdir, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")

    full = os.path.join(outdir, f"elm_trace_{stamp}.csv")
    with open(full, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["time", "header", "request", "kind", "response"])
        w.writeheader()
        w.writerows(LOG_ROWS)

    uniq = os.path.join(outdir, f"elm_pids_{stamp}.csv")
    with open(uniq, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["header", "mode", "pid", "full_request", "count"])
        for (hdr, req), cnt in SEEN.items():
            if req.startswith("AT") or req == "?":
                continue
            mode = req[:2]
            pid = req[2:]
            w.writerow([hdr, mode, pid, req, cnt])

    if PROBE_LOG:
        plog = os.path.join(outdir, f"probe_log_{stamp}.csv")
        with open(plog, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=["time", "iso", "slot", "idx", "header",
                                              "request", "nbytes", "hex", "value", "byte0"])
            w.writeheader()
            w.writerows(PROBE_LOG)
        print(f"\n探测日志: {plog}  ({len(PROBE_LOG)} 条注入记录，"
              f"{len(PROBE_IDX)} 个不同请求)")
        print("  下一步: python3 analyze_probe.py identify "
              f"--probe {plog} --app <CarScanner导出的.csv>")

    obd = [(h, r, c) for (h, r), c in SEEN.items() if not r.startswith("AT")]
    print(f"\n{'='*64}")
    print(f"共 {len(LOG_ROWS)} 条流水，去重后 {len(obd)} 个不同请求")
    print(f"{'='*64}")
    by_hdr = {}
    for h, r, c in obd:
        by_hdr.setdefault(h, []).append((r, c))
    for h in sorted(by_hdr):
        reqs = by_hdr[h]
        print(f"\n  header {h}  ({len(reqs)} 个)")
        for r, c in sorted(reqs):
            print(f"      {r:<14} x{c}")
    print(f"\n全量流水: {full}")
    print(f"去重 PID: {uniq}\n")


def selftest(port):
    time.sleep(0.8)
    print("\n--- selftest：本地自连测试 ---")
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
        s.settimeout(3)
        for c in ["ATZ", "ATE0", "ATH1", "ATS0", "0100", "0902", "ATSH 7E0", "22F190"]:
            s.sendall(c.encode() + b"\r")
            time.sleep(0.1)
            print(f"    {c:<12} -> {s.recv(4096).decode().strip()!r}")
        s.close()
        print("--- selftest 通过：服务端正常，问题在 app 侧或网络侧 ---\n")
    except OSError as e:
        print(f"--- selftest 失败: {e} ---\n")


def main():
    global VERBOSE
    p = argparse.ArgumentParser(description="伪 ELM327 WiFi 服务器，抓取 app 发出的所有请求")
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=35000)
    p.add_argument("--vin", default="WVWZZZAUZJW123456",
                   help="返回给 app 的 VIN，决定它自动识别成哪台车")
    p.add_argument("--calid", default="CALID0000000001")
    p.add_argument("--default-len", type=int, default=4,
                   help="未知 PID 返回几个数据字节（默认 4）")
    p.add_argument("--delay", type=float, default=0.01,
                   help="每条响应前的延时秒数，模拟真实适配器")
    p.add_argument("--nodata-unknown", action="store_true",
                   help="对完全未知的 mode 返回 NO DATA（默认返回假肯定响应）")
    p.add_argument("--outdir", default=".", help="CSV 输出目录")
    p.add_argument("--greet", choices=["none", "prompt", "banner"], default="prompt",
                   help="连上后是否主动出声：none=不发, prompt=发 '>', banner=发 ELM327 v1.5")
    p.add_argument("--idle-timeout", type=float, default=300.0,
                   help="多少秒没数据就回收这条连接")
    p.add_argument("--selftest", action="store_true",
                   help="启动后自己连自己跑一遍握手，验证服务端本身没问题")
    p.add_argument("--probe", choices=["off", "prbs", "basis"], default="off",
                   help="探测模式：prbs=并行识别哪个 PID 驱动哪个仪表；"
                        "basis=逐字节基向量探测，用来解公式")
    p.add_argument("--tick", type=float, default=2.0,
                   help="prbs 模式下信号多久换一次值（秒）")
    p.add_argument("--dwell", type=float, default=8.0,
                   help="basis 模式下每个图案保持多久（秒）")
    p.add_argument("--raw", action="store_true", help="十六进制打印收到的原始字节，排查用")
    p.add_argument("-v", "--verbose", action="store_true", help="打印每一条请求")
    cfg = p.parse_args()
    VERBOSE = cfg.verbose

    # 开工前先确认端口上没有别人（尤其是自己的僵尸进程）
    probe = socket.socket()
    probe.settimeout(0.4)
    try:
        probe.connect(("127.0.0.1", cfg.port))
        probe.close()
        print(f"""
  !! 端口 {cfg.port} 上已经有进程在监听了。
     macOS 会把连接随机分给多个监听者，旧进程会偷走一半流量，
     症状就是 tcpdump 看得到连接、但本进程一条都收不到。

     先查是谁：   lsof -nP -iTCP:{cfg.port} -sTCP:LISTEN
     全部清掉：   pkill -f fake_elm327.py
""")
        sys.exit(1)
    except (socket.timeout, ConnectionRefusedError, OSError):
        pass
    finally:
        probe.close()

    ip = "?"
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
    except OSError:
        pass

    print(f"""
  伪 ELM327 已启动  ->  {cfg.host}:{cfg.port}   (pid {os.getpid()})
  本机局域网 IP     ->  {ip}

  Car Scanner 设置：Settings → Connection → WiFi
      IP   = {ip}
      Port = {cfg.port}
  （手机和本机要在同一个 WiFi 下）

  连上后：手动选一个车型 profile，打开所有仪表页，跑一次 full scan。
  Ctrl-C 结束并导出 CSV。
""")
    if cfg.selftest:
        threading.Thread(target=selftest, args=(cfg.port,), daemon=True).start()
    try:
        serve(cfg)
    except KeyboardInterrupt:
        print("\n正在导出...")
    except OSError as e:
        print(f"\n  !! 绑定 {cfg.host}:{cfg.port} 失败: {e}"
              f"\n     lsof -nP -iTCP:{cfg.port} -sTCP:LISTEN   看看是谁占着\n")
    finally:
        dump(cfg.outdir)


if __name__ == "__main__":
    main()