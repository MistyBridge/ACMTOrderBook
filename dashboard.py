# -*- coding: utf-8 -*-
"""
OrderBook Reconstruction Engine - Comparison Dashboard

Design:
  - Top config bar: editable Python entry / C++ entry / data file paths
  - Two independent runner panels (Python blue, C++ green)
  - Each run always executes to completion; result same name -> overwrite silently
  - Bottom shared log pane
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, filedialog
import subprocess, threading, time, os, re, sys

# ──────────────────── paths ────────────────────
# When frozen by PyInstaller, __file__ points to a temp dir.
# Use the directory of the .exe (sys.executable) instead.
if getattr(sys, "frozen", False):
    PROJECT_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))

PY_EXE    = os.path.join(PROJECT_DIR, "py", "main.exe")
CPP_EXE   = os.path.join(PROJECT_DIR, "cpp", "orderbook.exe")
DATA_FILE = os.path.join(PROJECT_DIR, "data", "20220422", "AX_sbe_szse_000001.log")

# ──────────────────── theme ────────────────────
TH = {
    "bg":      "#1e1e2e",
    "panel":   "#282840",
    "accent1": "#74c7ec",   # Python
    "accent2": "#a6e3a1",   # C++
    "text":    "#cdd6f4",
    "dim":     "#6c7086",
    "bar_bg":  "#313244",
    "yellow":  "#f9e2af",
    "red":     "#f38ba8",
}


# ──────────────────── helpers ──────────────────
def _label(parent, **kw):
    kw.setdefault("bg", TH["panel"])
    return tk.Label(parent, **kw)


# ──────────────────── Runner ───────────────────
class Runner:
    """Represents one engine run (Python or C++)."""
    def __init__(self, name, accent):
        self.name   = name
        self.accent = accent
        self.running     = False
        self.progress    = 0.0
        self.speed       = ""
        self.elapsed     = 0.0
        self.result_lines = []
        self.start_time  = 0.0


# ──────────────────── Dashboard ────────────────
class Dashboard(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("OrderBook Reconstruction Engine · Comparison Dashboard")
        self.geometry("1100x750")
        self.configure(bg=TH["bg"])
        self.resizable(True, True)

        self._build_header()
        self._build_config()
        self._build_panels()
        self._build_log()
        self._tick()

    # ── header ──
    def _build_header(self):
        h = tk.Frame(self, bg=TH["bg"])
        h.pack(fill="x", padx=20, pady=(12, 4))
        tk.Label(h, text="OrderBook Reconstruction Engine",
                 font=("Consolas", 18, "bold"),
                 fg=TH["text"], bg=TH["bg"]).pack(side="left")
        tk.Label(h, text="  000001 | 2022-04-22 | 233,875 msgs",
                 font=("Consolas", 11), fg=TH["dim"], bg=TH["bg"]).pack(side="left", padx=15)

    # ── config bar ──
    def _build_config(self):
        fr = tk.Frame(self, bg=TH["panel"],
                      highlightbackground=TH["dim"], highlightthickness=1)
        fr.pack(fill="x", padx=20, pady=(0, 8))

        self.cfg_vars = {}
        rows = [
            ("Python 入口",  "py_main",  PY_EXE, True),
            ("C++ 入口",     "cpp_exe",  CPP_EXE, True),
            ("数据文件", "data",  DATA_FILE, True),
            ("重放次数", "replay",  "1", True),
        ]
        for i, (label, key, default, editable) in enumerate(rows):
            r = tk.Frame(fr, bg=TH["panel"])
            r.pack(fill="x", padx=12, pady=(8, 2) if i == 0 else 2)
            tk.Label(r, text=label+":", width=10, anchor="e",
                     font=("Consolas", 9), fg=TH["dim"], bg=TH["panel"]).pack(side="left")
            var = tk.StringVar(value=default)
            ent = tk.Entry(r, textvariable=var, font=("Consolas", 9),
                           bg="#1e1e2e", fg=TH["text"], insertbackground=TH["text"],
                           relief="flat", state="normal" if editable else "readonly")
            ent.pack(side="left", fill="x", expand=True, padx=(6, 6))
            if key != "replay":  # 重放次数不需要浏览按钮
                tk.Button(r, text="浏览", font=("Consolas", 8),
                          command=lambda v=var, k=key: self._browse(v, k)).pack(side="left")
            else:
                # 重放次数提示
                tk.Label(r, text="(1-1000)", font=("Consolas", 8),
                         fg=TH["dim"], bg=TH["panel"]).pack(side="left", padx=5)
            self.cfg_vars[key] = var

        # bottom padding
        tk.Frame(fr, bg=TH["panel"], height=6).pack(fill="x")

    def _browse(self, var, kind):
        init = os.path.dirname(var.get())
        if kind == "data":
            f = filedialog.askopenfilename(
                initialdir=init,
                filetypes=[("Log", "*.log"), ("All", "*.*")])
        else:
            f = filedialog.askopenfilename(initialdir=init)
        if f:
            var.set(f)

    # ── two panels ──
    def _build_panels(self):
        container = tk.Frame(self, bg=TH["bg"])
        container.pack(fill="both", expand=True, padx=20, pady=5)
        container.columnconfigure(0, weight=1)
        container.columnconfigure(1, weight=1)
        container.rowconfigure(0, weight=1)

        self.runners = {}
        for i, (key, name, accent) in enumerate([
            ("py",  "Python", TH["accent1"]),
            ("cpp", "C++",    TH["accent2"]),
        ]):
            r = Runner(name, accent)
            self.runners[key] = r
            pf = tk.Frame(container, bg=TH["panel"],
                          highlightbackground=TH["dim"], highlightthickness=1)
            pf.grid(row=0, column=i,
                    sticky="nsew",
                    padx=(0, 10) if i == 0 else (10, 0))
            self._build_one_panel(pf, r, key)

    def _build_one_panel(self, parent, r, key):
        # title
        tf = tk.Frame(parent, bg=TH["panel"])
        tf.pack(fill="x", padx=15, pady=(12, 4))
        r.dot = _label(tf, text=" ● ",
                       font=("Consolas", 16), fg=r.accent)
        r.dot.pack(side="left")
        _label(tf, text=r.name,
               font=("Consolas", 16, "bold"), fg=r.accent).pack(side="left")

        # button
        bf = tk.Frame(parent, bg=TH["panel"])
        bf.pack(fill="x", padx=15, pady=5)
        r.btn = tk.Button(bf,
                          text="▶ Run " + r.name,
                          font=("Consolas", 12, "bold"),
                          fg=TH["bg"], bg=r.accent,
                          relief="flat", cursor="hand2",
                          command=lambda: self._start_run(r, key))
        r.btn.pack(fill="x", ipady=6)

        # progress bar
        sname = r.name + ".Horizontal.TProgressbar"
        st = ttk.Style()
        st.theme_use("default")
        st.configure(sname, troughcolor=TH["bar_bg"],
                     background=r.accent, thickness=22, borderwidth=0)
        r.bar = ttk.Progressbar(parent, style=sname,
                                maximum=100, mode="determinate")
        r.bar.pack(fill="x", padx=15, pady=(8, 2))

        # pct / speed
        inf = tk.Frame(parent, bg=TH["panel"])
        inf.pack(fill="x", padx=15)
        r.pct = _label(inf, text="0.0%",
                       font=("Consolas", 20, "bold"), fg=r.accent)
        r.pct.pack(side="left")
        r.spd = _label(inf, text="waiting...",
                       font=("Consolas", 11), fg=TH["dim"])
        r.spd.pack(side="right")

        # stats grid
        sf = tk.Frame(parent, bg=TH["panel"])
        sf.pack(fill="x", padx=15, pady=(10, 5))
        r.stats = {}
        for j, (k, _) in enumerate([
            ("Time", "-"),   ("Trades", "-"), ("LastPx", "-"),
            ("Bid1", "-"),   ("Ask1", "-"),   ("Msg/s", "-"),
            ("P50", "-"),    ("P99", "-"),    ("Pmax", "-"),
        ]):
            ro, co = divmod(j, 3)
            f = tk.Frame(sf, bg=TH["panel"])
            f.grid(row=ro, column=co, sticky="w", padx=(0, 15), pady=2)
            _label(f, text=k, font=("Consolas", 9), fg=TH["dim"]).pack(side="left")
            v = _label(f, text="-", font=("Consolas", 10, "bold"), fg=TH["text"])
            v.pack(side="left", padx=(4, 0))
            r.stats[k] = v

    # ── log pane ──
    def _build_log(self):
        lf = tk.Frame(self, bg=TH["bg"])
        lf.pack(fill="both", expand=False, padx=20, pady=(0, 10))
        tk.Label(lf, text="Output Log",
                 font=("Consolas", 10), fg=TH["dim"], bg=TH["bg"]).pack(anchor="w")
        self.log = scrolledtext.ScrolledText(
            lf, height=8, font=("Consolas", 9),
            bg=TH["panel"], fg=TH["text"],
            relief="flat", state="disabled")
        self.log.pack(fill="both", expand=True, pady=(2, 0))

    def _log(self, msg):
        self.log.configure(state="normal")
        self.log.insert("end", msg + chr(10))
        self.log.see("end")
        self.log.configure(state="disabled")

    # ── run control ──
    def _start_run(self, r, key):
        """Always starts a fresh run (no cache check)."""
        if r.running:
            return

        # build command from current config values
        data = self.cfg_vars["data"].get()
        replay = self.cfg_vars["replay"].get()

        # 验证重放次数
        try:
            replay_count = int(replay)
            if replay_count < 1:
                replay_count = 1
            elif replay_count > 1000:
                replay_count = 1000
        except ValueError:
            replay_count = 1

        if key == "py":
            py_main = self.cfg_vars["py_main"].get()
            cmd = [py_main, data]
            cwd = PROJECT_DIR
            env = None
        else:
            cpp = self.cfg_vars["cpp_exe"].get()
            # C++ 可执行文件参数：data producerCore consumerCore queueCapacity batchSize replayCount
            cmd = [cpp, data, "0", "2", "16384", "64", str(replay_count)]
            cwd = os.path.dirname(cpp) or PROJECT_DIR
            env = None

        # check file exists
        if not os.path.isfile(cmd[0]):
            self._log(f"[{r.name}] ERROR: entry not found: {cmd[0]}")
            return
        if not os.path.isfile(data):
            self._log(f"[{r.name}] ERROR: data file not found: {data}")
            return

        # reset
        r.running = True
        r.progress = 0.0
        r.speed = ""
        r.elapsed = 0.0
        r.result_lines = []
        r.bar["value"] = 0
        r.pct.config(text="0.0%")
        r.spd.config(text="running...")
        r.btn.config(state="disabled", text="⏸ Running...")
        for v in r.stats.values():
            v.config(text="-")
        r.dot.config(fg=r.accent)

        self._log(f"\n{'='*60}")
        self._log(f"  [{r.name}] start  {time.strftime('%H:%M:%S')}")
        self._log(f"  CMD: {' '.join(cmd)}")
        if replay_count > 1:
            self._log(f"  Replay: {replay_count} times")
        self._log(f"{'='*60}")

        r.start_time = time.time()
        r._cmd, r._cwd, r._env = cmd, cwd, env
        r._replay_count = replay_count  # 存储重放次数用于进度计算
        threading.Thread(target=self._run_thread, args=(r,), daemon=True).start()

    def _run_thread(self, r):
        try:
            proc = subprocess.Popen(
                r._cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                cwd=r._cwd,
                bufsize=0,
                universal_newlines=True,
                env=r._env,
                creationflags=subprocess.CREATE_NO_WINDOW,
            )
            buf = ""
            while True:
                ch = proc.stdout.read(1)
                if not ch:
                    break
                if ch == chr(10):
                    line = buf.rstrip()
                    buf = ""
                    if line:
                        r.result_lines.append(line)
                        self.after(0, self._parse_line, r, line)
                else:
                    buf += ch
            if buf.strip():
                r.result_lines.append(buf.strip())
                self.after(0, self._parse_line, r, buf.strip())

            proc.wait()
            r.progress = 100.0

        except Exception as e:
            r.result_lines.append("ERROR: " + str(e))

        finally:
            r.running = False

    # ── line parser (regex-based, supports both py & cpp output) ──
    def _parse_line(self, r, line):
        # 计算总消息数（考虑重放次数）
        replay_count = getattr(r, '_replay_count', 1)
        total_msgs = 233875 * replay_count

        # progress (py: "  50,000 msgs  |  15.2s  |  3,289 msg/s")
        m = re.search(r"([\d,]+)\s+msgs?\s+\|\s+([\d.]+)s\s+\|\s+([\d,]+)\s+msg/s", line)
        if m:
            total_str = m.group(1).replace(",", "")
            try:
                total_now = int(total_str)
                r.progress = min(total_now / total_msgs * 100, 99.9)
            except ValueError:
                pass

        # cpp progress (cpp: "processed 50000 msgs ...")
        m = re.search(r"processed\s+(\d+)\s+msgs", line)
        if m:
            r.progress = min(int(m.group(1)) / total_msgs * 100, 99.9)

        # cpp progress (v2: "produced 50000 msgs...")
        m = re.search(r"produced\s+(\d+)\s+msgs", line)
        if m:
            r.progress = min(int(m.group(1)) / total_msgs * 100, 99.9)

        # timing (cpp: "Time: 2.5 s (98992 msg/s)")
        m2 = re.search(r"Time:\s+([\d.]+)\s*s\s*\((\d+)\s*msg/s\)", line)
        if m2:
            r.elapsed = float(m2.group(1))
            r.speed   = m2.group(2)

        # py final stats  (format: "  Time     :     75.140 s")
        #   also matches: "  Trades   :     81,049"
        for pat, key in [
            (r"NumTrades\s*=\s*([\d,]+)",               "Trades"),
            (r"LastPx\s*=\s*([\d.]+)",                  "LastPx"),
            (r"bidMax\s*=\s*(\d+)",                      "Bid1"),
            (r"askMin\s*=\s*(\d+)",                      "Ask1"),
            (r"Trades\s*:\s*([\d,]+)",                   "Trades"),
            (r"LastPx\s*:\s*([\d.]+)",                   "LastPx"),
            (r"Time\s*:\s*([\d.]+)\s*s",                 "Time"),
            (r"Speed\s*:\s*([\d,]+)\s*msg/s",            "Msg/s"),
        ]:
            m3 = re.search(pat, line)
            if m3:
                r.stats[key].config(text=m3.group(1))

        # latency (v2: "Latency: p50=1.2us p99=15.3us p99.9=45.6us pmax=120.0us")
        m_lat = re.search(r"Latency:\s*p50=([\d.]+)us\s*p99=([\d.]+)us\s*p99\.9=[\d.]+us\s*pmax=([\d.]+)us", line)
        if m_lat:
            r.stats["P50"].config(text=m_lat.group(1) + "us")
            r.stats["P99"].config(text=m_lat.group(2) + "us")
            r.stats["Pmax"].config(text=m_lat.group(3) + "us")

        # py 5-level orderbook  (format: "  16.0600     344,860")
        #   first ask line after "ASK" header  -> Ask1
        #   first bid line after "BID" header  -> Bid1
        if "ASK" in line and "^" in line:
            r._next_is_ask1 = True;  r._next_is_bid1 = False
        elif "BID" in line and "^" in line:
            r._next_is_ask1 = False; r._next_is_bid1 = True
        else:
            price_m = re.match(r"\s+([\d.]+)\s+([\d,]+)", line)
            if price_m:
                if getattr(r, "_next_is_ask1", False):
                    r.stats["Ask1"].config(text=price_m.group(1))
                    r._next_is_ask1 = False
                elif getattr(r, "_next_is_bid1", False):
                    r.stats["Bid1"].config(text=price_m.group(1))
                    r._next_is_bid1 = False

        # summary line -> log
        if "Total:" in line or "Messages" in line:
            self._log(f"  [{r.name}] {line.strip()}")

    # ── tick (UI refresh 100ms) ──
    def _tick(self):
        for r in self.runners.values():
            r.bar["value"] = r.progress
            r.pct.config(text=f"{r.progress:.1f}%")

            if r.running:
                el = time.time() - r.start_time
                if el > 0.5 and r.progress > 1:
                    sp = int(233875 * r.progress / 100 / el)
                    rm = el / (r.progress / 100) - el
                    r.spd.config(text=f"{sp:,} msg/s │ ETA {rm:.0f}s")
                    r.stats["Time"].config(text=f"{el:.1f}s")
                    r.stats["Msg/s"].config(text=f"{sp:,}")
                elif el > 0:
                    r.spd.config(text=f"running... {el:.1f}s")

            elif r.progress >= 100:
                # finished
                r.btn.config(state="normal", text="▶ Run " + r.name)
                sp = r.speed if r.speed else "?"
                et = str(round(r.elapsed, 2)) if r.elapsed else "?"
                r.spd.config(text=f"{sp} msg/s │ done in {et}s")
                r.stats["Time"].config(text=et + "s")
                r.stats["Msg/s"].config(text=sp)
                r.dot.config(fg=TH["yellow"])

            else:
                r.btn.config(state="normal", text="▶ Run " + r.name)

        self.after(100, self._tick)


if __name__ == "__main__":
    Dashboard().mainloop()
