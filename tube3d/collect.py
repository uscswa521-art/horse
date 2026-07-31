#!/usr/bin/env python3
"""
Tube3D — 攞 YouTube 片單, 寫成 data/videos.json 畀 3D 大堂用。

⚠️ 老實講清楚: **YouTube 冇任何 API 攞得到「你嘅個人化首頁」**。
   個人化推薦係登入狀態下嘅私人數據, 官方 Data API 冇呢個 endpoint。
   所以呢度攞嘅係公開嘅 **熱門片 (chart=mostPopular)**, 可以指定地區,
   另外可以自己加關鍵字 / 頻道。畫面上會照寫明係「熱門」唔係「你嘅首頁」。

quota: chart=mostPopular 一次只係 **1 unit** (好平, 20 分鐘更新一次都用唔晒),
       search 一次 100 units (所以預設唔開)。

跑法:
  python3 -m tube3d.collect --demo              # 示範數據, 即刻睇到效果
  YOUTUBE_API_KEY=xxx python3 -m tube3d.collect # 真數據
  python3 -m tube3d.collect --regions HK,US,JP --limit 60
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36")
API = "https://www.googleapis.com/youtube/v3/"

OUT_FILE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "data", "videos.json")


def _get(path: str, params: dict) -> dict:
    url = API + path + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def _iso_dur_secs(s: str) -> int:
    """PT1H2M3S → 3723 秒。攞唔到就 0。"""
    if not s or not s.startswith("PT"):
        return 0
    num, total = "", 0
    for ch in s[2:]:
        if ch.isdigit():
            num += ch
        else:
            v = int(num or 0)
            total += v * {"H": 3600, "M": 60, "S": 1}.get(ch, 0)
            num = ""
    return total


def fetch_region(key: str, region: str, limit: int) -> list:
    """熱門片 (1 quota unit)。"""
    out, token = [], None
    while len(out) < limit:
        p = {
            "part": "snippet,statistics,contentDetails",
            "chart": "mostPopular",
            "regionCode": region,
            "maxResults": min(50, limit - len(out)),
            "key": key,
        }
        if token:
            p["pageToken"] = token
        d = _get("videos", p)
        for v in d.get("items", []):
            sn, st = v.get("snippet") or {}, v.get("statistics") or {}
            th = (sn.get("thumbnails") or {})
            thumb = (th.get("medium") or th.get("high") or th.get("default") or {}).get("url", "")
            out.append({
                "id": v.get("id", ""),
                "title": sn.get("title", ""),
                "channel": sn.get("channelTitle", ""),
                "channel_id": sn.get("channelId", ""),
                "views": int(st.get("viewCount") or 0),
                "likes": int(st.get("likeCount") or 0),
                "published": sn.get("publishedAt", ""),
                "duration": _iso_dur_secs((v.get("contentDetails") or {}).get("duration", "")),
                "thumb": thumb,
                "url": f"https://www.youtube.com/watch?v={v.get('id','')}",
                "region": region,
                "live": (sn.get("liveBroadcastContent") or "none") != "none",
            })
        token = d.get("nextPageToken")
        if not token:
            break
    return out


def _videos_by_id(key: str, ids: list) -> list:
    """一次過攞一批 video 嘅資料 (1 unit / 50 條)。"""
    out = []
    for i in range(0, len(ids), 50):
        d = _get("videos", {
            "part": "snippet,statistics,contentDetails",
            "id": ",".join(ids[i:i+50]), "key": key,
        })
        for v in d.get("items", []):
            sn, st = v.get("snippet") or {}, v.get("statistics") or {}
            th = sn.get("thumbnails") or {}
            out.append({
                "id": v.get("id", ""),
                "title": sn.get("title", ""),
                "channel": sn.get("channelTitle", ""),
                "channel_id": sn.get("channelId", ""),
                "views": int(st.get("viewCount") or 0),
                "likes": int(st.get("likeCount") or 0),
                "published": sn.get("publishedAt", ""),
                "duration": _iso_dur_secs((v.get("contentDetails") or {}).get("duration", "")),
                "thumb": (th.get("medium") or th.get("high") or th.get("default") or {}).get("url", ""),
                "url": f"https://www.youtube.com/watch?v={v.get('id','')}",
                "region": "SUB",
                "live": (sn.get("liveBroadcastContent") or "none") != "none",
            })
    return out


def fetch_channels(key: str, chans: list, per: int, errors: dict = None) -> list:
    """
    跟開嘅頻道最新片 —— 呢個係最接近「你嘅首頁」嘅公開做法。
    每個頻道大約 3 units (channels + playlistItems + videos 攤分)。
    chans 入面可以係 channel id (UC…) 或者 handle (@name)。

    ⚠️ 邊個頻道攞唔到都會寫入 errors dict (再寫落 JSON, 畫面會顯示) —
       打錯 handle 嘅時候 API 係回 200 + 空 items, 唔記錄就會靜雞雞少咗嘢。
    """
    errors = {} if errors is None else errors
    ids = []
    for ch in chans:
        try:
            p = {"part": "contentDetails", "key": key}
            if ch.startswith("@"):
                p["forHandle"] = ch
            elif ch.startswith("UC"):
                p["id"] = ch
            else:
                p["forUsername"] = ch
            d = _get("channels", p)
            items = d.get("items") or []
            if not items:
                errors[ch] = "搵唔到呢個頻道 (handle / id 打錯咗?)"
                print(f"   ❌ 頻道 {ch}: {errors[ch]}")
                continue
            up = ((items[0].get("contentDetails") or {})
                  .get("relatedPlaylists") or {}).get("uploads")
            if not up:
                errors[ch] = "個頻道冇 uploads playlist"
                print(f"   ❌ 頻道 {ch}: {errors[ch]}")
                continue
            pl = _get("playlistItems", {
                "part": "contentDetails", "playlistId": up,
                "maxResults": min(50, per), "key": key,
            })
            ids += [(it.get("contentDetails") or {}).get("videoId")
                    for it in pl.get("items", [])
                    if (it.get("contentDetails") or {}).get("videoId")]
        except Exception as e:                                    # noqa: BLE001
            errors[ch] = f"{type(e).__name__}: {e}"
            print(f"   ❌ 頻道 {ch}: {errors[ch]}")
    return _videos_by_id(key, ids) if ids else []


def demo_videos(n: int = 40) -> list:
    """示範數據 — 標題全部虛構, 唔對應任何真實影片。"""
    kinds = [
        ("【4K】深夜城市散步 · 香港中環", "夜行頻道", 1_240_000, 1320),
        ("15 分鐘搞掂一餐：蒜香白酒炒蜆", "廚房日常", 842_000, 903),
        ("我用 100 蚊挑戰一星期伙食", "慳錢實驗室", 2_310_000, 1105),
        ("新機開箱：真係值得換？", "科技碎碎念", 566_000, 742),
        ("Lo-Fi 讀書歌單 · 3 小時不間斷", "Study Room", 4_120_000, 10800),
        ("由零學結他：第 1 課", "六條弦", 331_000, 1450),
        ("九龍城寨遺址：一段被拆走嘅歷史", "城市考古", 1_780_000, 1620),
        ("全馬破 3 小時，我做咗啲咩", "跑者手記", 245_000, 1180),
        ("貓貓第一次見到雪", "毛孩日常", 6_450_000, 186),
        ("拆解一部 20 年前嘅相機", "拆嘢頻道", 412_000, 1340),
        ("台南五日四夜，食到爆", "食旅日記", 987_000, 1520),
        ("Excel 十個你未用過嘅功能", "返工求生", 1_150_000, 880),
        ("凌晨三點嘅便利店", "短片劇場", 733_000, 420),
        ("由設計到落地：一張櫈嘅誕生", "做嘢的人", 198_000, 1260),
        ("小學雞數學題，成年人竟然答錯", "腦力操場", 3_020_000, 640),
        ("一個人搬屋，唔好學我", "生活碎片", 521_000, 1080),
        ("香港行山 · 大東山芒草季", "山系頻道", 664_000, 1390),
        ("我試咗一個月唔飲奶茶", "自我實驗", 1_460_000, 950),
        ("懷舊遊戲機修復實錄", "電子維修", 289_000, 1720),
        ("零基礎學水彩：畫一朵花", "顏料箱", 376_000, 1150),
    ]
    out = []
    for i in range(n):
        t, ch, v, dur = kinds[i % len(kinds)]
        rep = i // len(kinds)
        if rep:                       # 唔好撞名, 否則會俾 dedupe 食走
            t = f"{t}（第 {rep + 1} 集）"
        mult = 1.0 / (1 + i * 0.06)
        out.append({
            "id": "", "title": t, "channel": ch,
            "channel_id": "", "views": int(v * mult), "likes": int(v * mult * 0.03),
            "published": "", "duration": dur, "thumb": "",
            "url": "", "region": "DEMO", "live": i % 17 == 0,
        })
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="攞 YouTube 熱門片畀 3D 大堂用")
    ap.add_argument("--demo", action="store_true", help="唔出街, 淨係砌示範數據")
    ap.add_argument("--regions", default=os.environ.get("YT_REGIONS", "HK,US,JP"),
                    help="攞邊幾個地區嘅熱門 (逗號分隔), 每個地區 1 quota unit")
    ap.add_argument("--limit", type=int, default=int(os.environ.get("YT_LIMIT", 24)),
                    help="每個地區攞幾多條")
    ap.add_argument("--channels", default=os.environ.get("YT_CHANNELS", ""),
                    help="跟開嘅頻道 (UC… id 或者 @handle, 逗號分隔) — "
                         "最接近「你嘅首頁」嘅公開做法")
    ap.add_argument("--per-channel", type=int,
                    default=int(os.environ.get("YT_PER_CHANNEL", 6)),
                    help="每個頻道攞幾多條最新片")
    ap.add_argument("--out", default=OUT_FILE)
    args = ap.parse_args(argv)

    key = os.environ.get("YOUTUBE_API_KEY", "")
    regions = [r.strip().upper() for r in args.regions.split(",") if r.strip()]
    vids, errors, source = [], {}, "live"

    if args.demo or not key:
        if not args.demo:
            errors["youtube"] = "未設定 YOUTUBE_API_KEY"
            print("⚠️  冇 API key, 出示範數據 (source=demo)")
        vids, source = demo_videos(40), "demo"
    else:
        chans = [c.strip() for c in args.channels.split(",") if c.strip()]
        if chans:
            try:
                got = fetch_channels(key, chans, args.per_channel, errors)
                vids.extend(got)
                print(f"   ✅ 跟開嘅頻道: {len(got)} 條")
                if not got:
                    errors.setdefault("channels", "一個頻道都攞唔到片")
            except Exception as e:                                 # noqa: BLE001
                errors["channels"] = f"{type(e).__name__}: {e}"
        for rg in regions:
            try:
                got = fetch_region(key, rg, args.limit)
                vids.extend(got)
                print(f"   ✅ {rg}: {len(got)} 條")
            except urllib.error.HTTPError as e:
                body = ""
                try:
                    body = e.read().decode("utf-8", "replace")[:160]
                except Exception:                                  # noqa: BLE001
                    pass
                hint = " (quota 爆咗?)" if e.code == 403 else ""
                errors[rg] = f"HTTP {e.code}{hint} {body}"
                print(f"   ❌ {rg}: {errors[rg]}")
            except Exception as e:                                 # noqa: BLE001
                errors[rg] = f"{type(e).__name__}: {e}"
                print(f"   ❌ {rg}: {errors[rg]}")
        if not vids:
            print("⚠️  一條都攞唔到, 改用示範數據 (source=demo)")
            vids, source = demo_videos(40), "demo"
        elif errors:
            source = "mixed"

    # dedupe (同一條片可能喺幾個地區都上榜) + 排序
    seen, uniq = set(), []
    for v in vids:
        k = v["id"] or (v["title"], v["channel"])
        if k in seen:
            continue
        seen.add(k)
        uniq.append(v)

    snap = {
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": source,
        # ⚠️ 呢句好重要, 畫面會照樣顯示出嚟
        # ⚠️ 出咗示範數據就唔可以再話係頻道 / 熱門榜 — 要照實講。
        "feed": ("demo" if source == "demo"
                 else ("channels+trending" if args.channels else "youtube_trending")),
        "note": ("呢啲係示範數據（標題全部虛構），唔係真實 YouTube 內容。"
                 if source == "demo" else
                 "YouTube 冇公開 API 攞個人化首頁。呢度顯示嘅係"
                 + ("你指定嘅頻道最新片 + " if args.channels else "")
                 + "公開熱門榜。"),
        "regions": regions if source != "demo" else ["DEMO"],
        "count": len(uniq),
        "errors": errors,
        "videos": uniq,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(snap, f, ensure_ascii=False, separators=(",", ":"))
        f.write("\n")
    print(f"✅ 寫咗 {args.out} — {len(uniq)} 條片, 來源 {source}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
