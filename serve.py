#!/usr/bin/env python3
"""
本機開嚟睇 / Run it locally.

一句嘢搞掂:  python3 serve.py

佢會:
  1. 睇下 data/*.json 有冇, 冇就即刻整份示範數據
     (有 API key 喺環境變數度就攞真數據)
  2. 起一個本機 http server (自動搵個得閒嘅 port)
  3. 開個瀏覽器彈出嚟

⚠️ 點解要開 server 唔直接 double-click 個 HTML?
   `file://` 之下瀏覽器會擋 fetch(), 讀唔到 data/*.json。
   （真係想 double-click 都得 —— 兩版都有 file:// 嘅 fallback,
     會用內建示範數據, 畫面會寫明。但係要真數據就一定要行呢個 server。）

用法:
  python3 serve.py                 # 預設開影片大堂
  python3 serve.py arena           # 開演唱會場館
  python3 serve.py wall            # 開直播主牆
  python3 serve.py --port 8000     # 指定 port
  python3 serve.py --no-open       # 唔好自動開瀏覽器
  python3 serve.py --live          # 唔好用示範數據, 逼佢出街攞真數據
"""
import argparse
import functools
import http.server
import os
import socket
import socketserver
import subprocess
import sys
import threading
import time
import webbrowser

ROOT = os.path.dirname(os.path.abspath(__file__))

PAGES = {
    "tube":  ("tube3d.html", "🎬 影片大堂 — 操控角色行入去揀片"),
    "arena": ("arena.html", "🎤 演唱會場館 — 全球直播人潮"),
    "wall":  ("arena.html?mode=wall", "🧱 直播主牆"),
    "overlay": ("arena.html?mode=overlay&cycle=0", "📺 OBS overlay 橫幅"),
}


def _free_port(want: int) -> int:
    """want 用得就用 want, 唔得就搵個得閒嘅。"""
    for p in [want] + list(range(want + 1, want + 40)):
        with socket.socket() as s:
            try:
                s.bind(("127.0.0.1", p))
                return p
            except OSError:
                continue
    return 0        # 交返畀 OS 揀


def _run(mod: str, args: list) -> bool:
    print(f"   → python3 -m {mod} {' '.join(args)}")
    r = subprocess.run([sys.executable, "-m", mod] + args, cwd=ROOT)
    return r.returncode == 0


def ensure_data(live: bool) -> None:
    """冇數據就整份出嚟。有 key 就試真數據, 冇就示範數據。"""
    jobs = [
        ("data/streamers.json", "streamer_arena.collect",
         bool(os.environ.get("TWITCH_CLIENT_ID") or os.environ.get("YOUTUBE_API_KEY"))),
        ("data/videos.json", "tube3d.collect",
         bool(os.environ.get("YOUTUBE_API_KEY"))),
    ]
    for path, mod, has_key in jobs:
        full = os.path.join(ROOT, path)
        if os.path.exists(full) and not live:
            print(f"✅ {path} 已經有")
            continue
        if has_key or live:
            print(f"📡 攞緊真數據 → {path}")
            if _run(mod, []):
                continue
            print(f"⚠️  攞唔到, 改用示範數據")
        else:
            print(f"🎭 冇 API key, 整份示範數據 → {path}")
        _run(mod, ["--demo"])


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        # 本機開發: 唔好 cache, 改完刷新即刻見到
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *a):        # 靜啲, 唔好洗版
        pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="本機開嚟睇")
    ap.add_argument("page", nargs="?", default="tube", choices=list(PAGES),
                    help="開邊版 (預設 tube)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-open", action="store_true", help="唔好自動開瀏覽器")
    ap.add_argument("--live", action="store_true", help="逼佢出街攞真數據")
    args = ap.parse_args(argv)

    print("── 準備數據 ──────────────────────────────")
    ensure_data(args.live)

    port = _free_port(args.port)
    socketserver.TCPServer.allow_reuse_address = True
    handler = functools.partial(Handler, directory=ROOT)
    with socketserver.TCPServer(("127.0.0.1", port), handler) as httpd:
        port = httpd.server_address[1]
        base = f"http://localhost:{port}"
        print("\n── 開好喇 ────────────────────────────────")
        for k, (path, desc) in PAGES.items():
            mark = "👉" if k == args.page else "  "
            print(f" {mark} {desc}\n      {base}/{path}")
        print("\n   Ctrl+C 收工\n")

        if not args.no_open:
            url = f"{base}/{PAGES[args.page][0]}"
            threading.Timer(0.6, lambda: webbrowser.open(url)).start()
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n收工 👋")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
