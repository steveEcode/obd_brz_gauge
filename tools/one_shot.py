#!/usr/bin/env python3
"""
one_shot.py —— 一键全流程：起伪 ELM327 服务器 → 你录一遍 → 自动出结果。

用法:
    python3 one_shot.py

它会:
  1. 后台启动 fake_elm327.py --probe prbs --tick 2（单遍采集，无需 basis）
  2. 打印手机 Car Scanner 的操作指引
  3. 等你录完、导出 CSV，回来粘贴路径（或直接回车自动找）；中途 Ctrl+C 也会停服务器并尝试分析
  4. 停掉服务器（触发它导出 probe_log / elm_pids）
  5. 调 analyze_probe.py auto 一条命令出 pids_full.csv
"""

import glob
import os
import signal
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def newest(pattern):
    cands = sorted(glob.glob(pattern))
    return cands[-1] if cands else None


def main():
    os.chdir(HERE)

    print("=" * 72)
    print("  一键倒推 Car Scanner PID")
    print("=" * 72)

    # 1) 起服务器
    print("\n[1/4] 启动伪 ELM327 服务器（probe=prbs，单遍采集）...\n")
    proc = subprocess.Popen(
        [sys.executable, os.path.join(HERE, "fake_elm327.py"),
         "--probe", "prbs", "--tick", "2"],
        cwd=HERE,
        start_new_session=True)  # 脱离终端进程组：Ctrl+C 只发给 one_shot，不打断子进程 dump

    app_csv = ""
    interrupted = False
    try:
        # 2) 等用户录
        print("\n[2/4] 手机操作：")
        print("   Car Scanner → Settings → Connection → WiFi")
        print("   填上面服务器打印的 IP 和端口 → 选车型 profile → 打开所有仪表页 → 开始记录")
        print("   录 10~15 分钟 → 导出 CSV 到本目录（或记下路径）")
        print("   （中途想停：直接 Ctrl+C，会用已录数据尝试分析）\n")
        app_csv = input("   录完导出后，把 CSV 路径粘过来（直接回车则自动找 *.csv）: ").strip()
        if app_csv:
            app_csv = app_csv.strip('"').strip("'")
    except KeyboardInterrupt:
        interrupted = True
        print("\n\n  ⚠ 检测到 Ctrl+C，提前结束录制，尝试用已录数据分析...")
    finally:
        # 3) 停服务器（触发它导出 probe_log / elm_pids）
        #    子进程已 start_new_session，终端 Ctrl+C 不会直接打到它；这里由我们
        #    干净地发一次 SIGINT 让它 dump 导出。send_signal 容忍「进程已死」。
        print("\n[3/4] 停止服务器并导出探测日志...")
        try:
            proc.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    plog = newest(os.path.join(HERE, "probe_log_*.csv"))
    pids = newest(os.path.join(HERE, "elm_pids_*.csv"))
    if not plog or not pids:
        print("\n  ✗ 没有生成探测日志 / 请求清单。")
        print("    原因几乎都是：手机 Car Scanner 没连上服务器（或没发任何 OBD 请求）。")
        print("    排查：")
        print("      · 手机和电脑是否在同一 WiFi")
        print("      · Car Scanner 里 IP / 端口填对了没（端口默认 35000）")
        print("      · 连上后有没有选车型、打开仪表页并开始记录")
        sys.exit(1)

    if not app_csv:
        non_tool = [
            c for c in sorted(glob.glob(os.path.join(HERE, "*.csv")))
            if not os.path.basename(c).startswith(
                ("probe_log_", "elm_pids_", "elm_trace_"))
        ]
        if len(non_tool) == 1:
            app_csv = non_tool[0]
        elif non_tool:
            print("\n本目录下这些 CSV 可能是 Car Scanner 导出的：")
            for i, c in enumerate(non_tool):
                print(f"  [{i}] {os.path.basename(c)}")
            sel = input("选一个编号: ").strip()
            app_csv = non_tool[int(sel)] if sel.isdigit() and int(sel) < len(non_tool) else non_tool[0]
        else:
            if interrupted:
                print("\n还没导出 Car Scanner 的 CSV。先把记录导出到本目录，然后跑：")
                print(f"   python3 analyze_proble.py auto "
                      f"--probe {os.path.basename(plog)} --app <导出的.csv>")
                sys.exit(0)
            app_csv = input("没找到候选 CSV，请手动粘贴路径: ").strip()

    if not app_csv or not os.path.exists(app_csv):
        print(f"app CSV 不存在: {app_csv}")
        sys.exit(1)

    # 4) 一键分析
    print(f"\n[4/4] 分析中... probe={os.path.basename(plog)}, app={os.path.basename(app_csv)}\n")
    r = subprocess.run(
        [sys.executable, os.path.join(HERE, "analyze_proble.py"), "auto",
         "--probe", plog, "--app", app_csv, "--pids", pids])
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
