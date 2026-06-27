#!/usr/bin/env python3
"""
搵屋爬蟲主程式 / Property finder — main entry.

做嘅嘢:
  1. 掃描多個租屋網 (rentals.ca / kijiji / zumper / padmapper ...)
  2. 過濾: 2 房 / 2 廁 / ≤ $2,800 / 近大班旅遊 (Markham)
  3. 同上次比較, 搵出「新」嘅筍盤
  4. Telegram 推送新筍盤
  5. 每次跑都發一個匯報 (即使冇新嘢都會講「今次冇」)

跑法:
  一次:   python3 -m property_finder.finder
  長駐:   python3 -m property_finder.finder --loop      (每 1 小時跑一次)
  GitHub Actions: 見 .github/workflows/property-finder.yml (cron 每小時)
"""
import argparse
import json
import os
import sys
import time
from datetime import datetime, timedelta, timezone

# 支援直接 `python3 finder.py` 或 `python3 -m property_finder.finder`
try:
    from . import config, scrapers
    from .telegram_notify import Telegram, esc
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from property_finder import config, scrapers
    from property_finder.telegram_notify import Telegram, esc

TORONTO_TZ = timezone(timedelta(hours=-4))  # America/Toronto (EDT, 夏令時)


# ── 過濾 / Filtering ──────────────────────────────────────────────────────
def matches(lst: "scrapers.Listing", cfg: dict) -> bool:
    if lst.bedrooms is not None:
        if lst.bedrooms < cfg["bedrooms_min"]:
            return False
        if cfg["bedrooms_max"] and lst.bedrooms > cfg["bedrooms_max"]:
            return False
    if lst.bathrooms is not None and lst.bathrooms < cfg["bathrooms_min"]:
        return False
    if lst.price is not None:
        # 過濾走明顯唔合理嘅 (例如手抽錯位/年租)
        if lst.price > cfg["price_max"] or lst.price < cfg["price_min"]:
            return False

    # 地點: 有座標就用距離, 冇就用關鍵字
    dist = scrapers.distance_km(
        lst.lat, lst.lng, cfg["office_lat"], cfg["office_lng"])
    if dist is not None:
        return dist <= cfg["radius_km"]
    blob = lst.text_blob()
    return any(kw in blob for kw in cfg["location_keywords"])


# ── 狀態檔 / Seen-state ───────────────────────────────────────────────────
def load_seen(path: str) -> dict:
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def save_seen(path: str, seen: dict):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # 淨係保留近 60 日, 避免無限膨脹
    cutoff = (datetime.now(timezone.utc) - timedelta(days=60)).isoformat()
    seen = {k: v for k, v in seen.items() if v.get("ts", "") >= cutoff}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(seen, f, ensure_ascii=False, indent=2)


# ── 訊息格式 / Message formatting ─────────────────────────────────────────
def fmt_listing(lst: "scrapers.Listing", cfg: dict) -> str:
    price = f"${int(lst.price):,}/月" if lst.price else "租金未列"
    beds = f"{int(lst.bedrooms)}房" if lst.bedrooms else "?房"
    baths = f"{int(lst.bathrooms)}廁" if lst.bathrooms else "?廁"
    dist = scrapers.distance_km(
        lst.lat, lst.lng, cfg["office_lat"], cfg["office_lng"])
    dist_s = f" · 距公司 {dist:.1f}km" if dist is not None else ""
    lines = [
        f"🏠 <b>{esc(lst.title or lst.address or '租盤')}</b>",
        f"💰 {esc(price)}　🛏 {beds} 🚿 {baths}{esc(dist_s)}",
    ]
    if lst.address:
        lines.append(f"📍 {esc(lst.address)}")
    lines.append(f"🔗 {esc(lst.url)}" if lst.url else "")
    lines.append(f"<i>來源: {esc(lst.source)}</i>")
    return "\n".join(x for x in lines if x)


def run_once(tg: Telegram, cfg: dict, state_path: str) -> dict:
    now = datetime.now(TORONTO_TZ)
    stamp = now.strftime("%Y-%m-%d %H:%M")
    seen = load_seen(state_path)

    all_listings, scanned_ok, errors = [], [], []
    for name, fn in scrapers.SOURCES:
        try:
            got = fn(cfg)
            all_listings.extend(got)
            scanned_ok.append(f"{name}({len(got)})")
            print(f"[{name}] 攞到 {len(got)} 個盤")
        except Exception as e:  # noqa: BLE001
            errors.append(f"{name}: {type(e).__name__}")
            print(f"[{name}] 失敗: {e}")

    # 過濾 + dedupe
    matched, new_listings = {}, []
    for lst in all_listings:
        if not matches(lst, cfg):
            continue
        uid = lst.uid()
        if uid in matched:
            continue
        matched[uid] = lst
        if uid not in seen:
            new_listings.append(lst)

    # 推送新筍盤
    for lst in new_listings:
        if tg.send(fmt_listing(lst, cfg)):
            seen[lst.uid()] = {
                "ts": datetime.now(timezone.utc).isoformat(),
                "title": lst.title,
                "url": lst.url,
            }

    # 每小時匯報 (一定發)
    report = build_report(stamp, scanned_ok, errors, len(matched),
                          new_listings, cfg)
    tg.send(report, disable_preview=True)

    save_seen(state_path, seen)
    print(f"[完成] {stamp} 符合 {len(matched)} / 新 {len(new_listings)}")
    return {"matched": len(matched), "new": len(new_listings),
            "errors": errors}


def build_report(stamp, scanned_ok, errors, n_match, new_listings, cfg) -> str:
    head = (
        f"🕐 <b>搵屋匯報</b> {esc(stamp)} (Markham)\n"
        f"條件: 租 · {cfg['bedrooms_min']}房{cfg['bathrooms_min']}廁 · "
        f"≤${cfg['price_max']:,}/月 · 近大班旅遊\n"
        f"━━━━━━━━━━━━━━━"
    )
    if new_listings:
        body = f"\n✅ <b>今次有 {len(new_listings)} 個新筍盤!</b> (上面已逐個推送)"
    elif n_match:
        body = f"\n😐 今次冇新嘢, 有 {n_match} 個之前匯報過嘅盤仍在。"
    else:
        body = "\n😴 今次冇搵到符合條件嘅盤。下個鐘再試。"
    src = "\n📡 已掃: " + (", ".join(scanned_ok) if scanned_ok else "（全部失敗）")
    err = f"\n⚠️ 出錯: {', '.join(errors)}" if errors else ""
    return head + body + src + err


# ── 入口 / Entry ──────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="搵屋爬蟲 → Telegram")
    ap.add_argument("--loop", action="store_true",
                    help="長駐模式, 每小時跑一次")
    ap.add_argument("--interval", type=int, default=3600,
                    help="--loop 嘅間隔秒數 (預設 3600 = 1 小時)")
    args = ap.parse_args()

    cfg = config.SEARCH
    tg = Telegram(config.TELEGRAM["bot_token"], config.TELEGRAM["chat_id"])
    tg.resolve_chat_id()
    state_path = config.STATE_FILE

    if args.loop:
        print(f"長駐模式: 每 {args.interval}s 跑一次。Ctrl+C 停止。")
        while True:
            try:
                run_once(tg, cfg, state_path)
            except Exception as e:  # noqa: BLE001
                print(f"[loop] 今次出錯: {e}")
                try:
                    tg.send(f"⚠️ 搵屋程式今次出錯: {esc(type(e).__name__)}")
                except Exception:  # noqa: BLE001
                    pass
            time.sleep(args.interval)
    else:
        run_once(tg, cfg, state_path)


if __name__ == "__main__":
    main()
