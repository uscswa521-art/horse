#!/usr/bin/env python3
"""
Streamer Arena — 收數程式 / snapshot collector.

做嘅嘢:
  1. 同時問 Twitch / YouTube / Kick / TikTok 攞「而家有幾多人睇緊」
  2. 合併、排序、寫成 data/streamers.json (arena.html 會 fetch 佢)
  3. 每個 snapshot 嘅總人數寫入 data/history.json (畫人潮走勢用)
  4. 邊個平台失敗都會誠實寫入 JSON, 前端會顯示「呢個平台攞唔到數據」

跑法:
  python3 -m streamer_arena.collect            # 有幾多 key 用幾多
  python3 -m streamer_arena.collect --demo     # 唔出街, 淨係砌示範數據
  python3 -m streamer_arena.collect --top 100 --out data/streamers.json
  GitHub Actions: 見 .github/workflows/streamer-arena.yml
"""
import argparse
import json
import os
import sys
from datetime import datetime, timezone

try:
    from . import config, demo, sources
except ImportError:   # 支援 python3 streamer_arena/collect.py
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from streamer_arena import config, demo, sources


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def build_snapshot(results: list, source: str) -> dict:
    """由 list[Result] 砌出前端食嘅 JSON。"""
    platforms, all_streamers = {}, []
    for r in results:
        vs = sum(s.viewers for s in r.streamers)
        platforms[r.platform] = {
            "ok": r.ok,
            "error": r.error,
            "streamers": len(r.streamers),
            "viewers": vs,
            "top": r.streamers[0].name if r.streamers else "",
        }
        all_streamers.extend(r.streamers)

    all_streamers.sort(key=lambda s: s.viewers, reverse=True)
    ok_names = [p for p, v in platforms.items() if v["ok"]]
    return {
        "generated_at": _now_iso(),
        # live = 真數據 / demo = 示範數據 / mixed = 部分平台攞到
        "source": source,
        "total_viewers": sum(s.viewers for s in all_streamers),
        "counted_streamers": len(all_streamers),
        "platforms_ok": ok_names,
        "platforms": platforms,
        "streamers": [s.to_dict() for s in all_streamers],
    }


def update_history(snapshot: dict, path: str, keep: int) -> None:
    hist = []
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                hist = json.load(f) or []
        except (OSError, ValueError):
            hist = []
    hist.append({
        "t": snapshot["generated_at"],
        "total": snapshot["total_viewers"],
        "source": snapshot["source"],
        # ⚠️ 用 .get: 「全部平台失敗」嗰條路會為未知平台砌一個冇 viewers 嘅 entry,
        #    直接 v["viewers"] 會 KeyError, 成個 run 死埋, 連寫好嘅 snapshot 都 commit 唔到。
        "platforms": {p: v.get("viewers", 0) for p, v in snapshot["platforms"].items()},
    })
    hist = hist[-keep:]
    _write_json(path, hist)


def _write_json(path: str, obj) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="收集全球直播主即時觀看人數")
    ap.add_argument("--demo", action="store_true",
                    help="唔出街, 直接砌示範數據 (測試個視覺化用)")
    ap.add_argument("--top", type=int, default=config.TOP_N,
                    help=f"每個平台攞幾多個 (預設 {config.TOP_N})")
    ap.add_argument("--platforms", default=",".join(config.PLATFORMS))
    ap.add_argument("--out", default=config.OUT_FILE)
    ap.add_argument("--history", default=config.HISTORY_FILE)
    ap.add_argument("--no-history", action="store_true")
    args = ap.parse_args(argv)

    plats = [p.strip() for p in args.platforms.split(",") if p.strip()]

    if args.demo:
        results, source = demo.demo_results(plats, args.top), "demo"
    else:
        results = sources.fetch_all(plats, args.top)
        got = [r for r in results if r.ok and r.streamers]
        if not got:
            # 一個平台都攞唔到 — 唔好出個空白畫面, 出示範數據但誠實標明
            print("⚠️  全部平台都攞唔到數據, 改用示範數據 (source=demo):")
            for r in results:
                print(f"   · {r.platform}: {r.error}")
            failed = results
            results = demo.demo_results(plats, args.top)
            snapshot = build_snapshot(results, "demo")
            # 保留真實嘅失敗原因, 唔好扮成功
            for r in failed:
                snapshot["platforms"].setdefault(r.platform, {})
                snapshot["platforms"][r.platform].update(
                    {"ok": False, "error": r.error, "demo": True})
            snapshot["platforms_ok"] = []
            _write_json(args.out, snapshot)
            if not args.no_history:
                update_history(snapshot, args.history, config.HISTORY_KEEP)
            _report(snapshot, args.out)
            return 0
        source = "live" if len(got) == len(results) else "mixed"

    snapshot = build_snapshot(results, source)
    _write_json(args.out, snapshot)
    if not args.no_history:
        update_history(snapshot, args.history, config.HISTORY_KEEP)
    _report(snapshot, args.out)
    return 0


def _report(snapshot: dict, out: str) -> None:
    print(f"✅ 寫咗 {out}")
    print(f"   時間: {snapshot['generated_at']}  來源: {snapshot['source']}")
    print(f"   總人數: {snapshot['total_viewers']:,}"
          f"  ({snapshot['counted_streamers']} 個直播)")
    for p, v in snapshot["platforms"].items():
        mark = "✅" if v.get("ok") else "❌"
        if v.get("ok"):
            print(f"   {mark} {p:<8} {v['viewers']:>10,} 人 / {v['streamers']} 個直播")
        else:
            print(f"   {mark} {p:<8} {v.get('error','')}")


if __name__ == "__main__":
    raise SystemExit(main())
