"""
各平台直播人數抓取 / Per-platform live viewer fetchers.

原則 (同 property_finder 一樣):
  · 每個平台獨立 try/except — 一個死咗唔會拖冧其他
  · 攞唔到就誠實報 error, 唔會靜雞雞當冇事
  · 全部 fetcher 都回傳同一個 Streamer 格式, 落到前端就一視同仁
"""
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass, field
from typing import Optional

from . import config

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
)


# ── 資料模型 / Data model ──────────────────────────────────────────────────
@dataclass
class Streamer:
    platform: str            # twitch / youtube / kick / tiktok
    name: str                # 顯示名
    viewers: int             # 即時觀看人數
    title: str = ""
    category: str = ""       # 打緊咩 game / 分類
    url: str = ""
    language: str = ""
    avatar: str = ""
    channel_id: str = ""     # 平台內部 id / slug, 用嚟做 highlight

    def uid(self) -> str:
        return f"{self.platform}:{self.channel_id or self.name}".lower()

    def to_dict(self) -> dict:
        d = asdict(self)
        d["id"] = self.uid()
        return d


@dataclass
class Result:
    """一個平台跑完之後嘅結果 (成功與否都有交代)。"""
    platform: str
    streamers: list = field(default_factory=list)
    ok: bool = True
    error: str = ""


def _log(*a):
    if config.DEBUG:
        print("[arena]", *a, flush=True)


# ── HTTP 小工具 ───────────────────────────────────────────────────────────
def _request(url: str, headers: dict = None, data: bytes = None,
             method: str = None) -> str:
    hdrs = {"User-Agent": UA, "Accept": "application/json, text/html;q=0.9"}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    with urllib.request.urlopen(req, timeout=config.HTTP_TIMEOUT) as r:
        return r.read().decode("utf-8", "replace")


def _get_json(url: str, headers: dict = None) -> dict:
    return json.loads(_request(url, headers=headers))


def _post_json(url: str, form: dict, headers: dict = None) -> dict:
    body = urllib.parse.urlencode(form).encode()
    hdrs = {"Content-Type": "application/x-www-form-urlencoded"}
    if headers:
        hdrs.update(headers)
    return json.loads(_request(url, headers=hdrs, data=body, method="POST"))


# ── Twitch ────────────────────────────────────────────────────────────────
def _twitch_token(client_id: str, client_secret: str) -> str:
    d = _post_json("https://id.twitch.tv/oauth2/token", {
        "client_id": client_id,
        "client_secret": client_secret,
        "grant_type": "client_credentials",
    })
    return d.get("access_token", "")


def fetch_twitch(top_n: int) -> Result:
    """Helix /streams — 一 page 100 個, 已經係按人數由多到少排。"""
    cid, secret = config.TWITCH["client_id"], config.TWITCH["client_secret"]
    if not (cid and secret):
        return Result("twitch", ok=False,
                      error="未設定 TWITCH_CLIENT_ID / TWITCH_CLIENT_SECRET")
    try:
        token = _twitch_token(cid, secret)
        if not token:
            return Result("twitch", ok=False, error="攞唔到 app access token")
        hdrs = {"Client-Id": cid, "Authorization": f"Bearer {token}"}

        out, cursor = [], None
        while len(out) < top_n:
            page = min(100, top_n - len(out))
            url = f"https://api.twitch.tv/helix/streams?first={page}"
            if cursor:
                url += f"&after={urllib.parse.quote(cursor)}"
            d = _get_json(url, headers=hdrs)
            data = d.get("data") or []
            if not data:
                break
            for s in data:
                out.append(Streamer(
                    platform="twitch",
                    name=s.get("user_name") or s.get("user_login") or "",
                    viewers=int(s.get("viewer_count") or 0),
                    title=s.get("title") or "",
                    category=s.get("game_name") or "",
                    url=f"https://twitch.tv/{s.get('user_login','')}",
                    language=s.get("language") or "",
                    avatar=(s.get("thumbnail_url") or "")
                    .replace("{width}", "160").replace("{height}", "90"),
                    channel_id=s.get("user_login") or "",
                ))
            cursor = (d.get("pagination") or {}).get("cursor")
            if not cursor:
                break
        _log("twitch:", len(out))
        return Result("twitch", streamers=out)
    except Exception as e:                                    # noqa: BLE001
        return Result("twitch", ok=False, error=f"{type(e).__name__}: {e}")


# ── YouTube ───────────────────────────────────────────────────────────────
def fetch_youtube(top_n: int) -> Result:
    """
    search (eventType=live, order=viewCount) 攞 video id,
    再用 videos?part=liveStreamingDetails 攞真正 concurrentViewers。
    search 貴 (100 units), 所以只做一次, 最多攞 50 個。
    """
    key = config.YOUTUBE["api_key"]
    if not key:
        return Result("youtube", ok=False, error="未設定 YOUTUBE_API_KEY")
    try:
        n = min(50, top_n)
        params = {
            "part": "id", "eventType": "live", "type": "video",
            "order": "viewCount", "maxResults": n, "key": key,
        }
        q = config.YOUTUBE["query"]
        # q 係必需嘅 (order=viewCount 冇 q 會回空), 留空就用一個好闊嘅字
        params["q"] = q or "live"
        if config.YOUTUBE["region"]:
            params["regionCode"] = config.YOUTUBE["region"]
        d = _get_json("https://www.googleapis.com/youtube/v3/search?"
                      + urllib.parse.urlencode(params))
        ids = [it["id"]["videoId"] for it in d.get("items", [])
               if it.get("id", {}).get("videoId")]
        if not ids:
            return Result("youtube", ok=False, error="search 冇回任何 live video")

        d2 = _get_json(
            "https://www.googleapis.com/youtube/v3/videos?"
            + urllib.parse.urlencode({
                "part": "snippet,liveStreamingDetails",
                "id": ",".join(ids), "key": key,
            }))
        out = []
        for v in d2.get("items", []):
            sn = v.get("snippet") or {}
            ls = v.get("liveStreamingDetails") or {}
            cc = ls.get("concurrentViewers")
            if cc is None:          # 已經收咗檔嘅唔計
                continue
            thumbs = sn.get("thumbnails") or {}
            out.append(Streamer(
                platform="youtube",
                name=sn.get("channelTitle") or "",
                viewers=int(cc),
                title=sn.get("title") or "",
                category=sn.get("liveBroadcastContent") or "live",
                url=f"https://youtube.com/watch?v={v.get('id','')}",
                language=sn.get("defaultAudioLanguage") or "",
                avatar=(thumbs.get("medium") or thumbs.get("default")
                        or {}).get("url", ""),
                channel_id=sn.get("channelId") or "",
            ))
        out.sort(key=lambda s: s.viewers, reverse=True)
        _log("youtube:", len(out))
        return Result("youtube", streamers=out)
    except urllib.error.HTTPError as e:
        body = ""
        try:
            body = e.read().decode("utf-8", "replace")[:200]
        except Exception:                                     # noqa: BLE001
            pass
        hint = " (好可能係 quota 爆咗, 唔好將 cron 改到太密)" if e.code == 403 else ""
        return Result("youtube", ok=False, error=f"HTTP {e.code}{hint} {body}")
    except Exception as e:                                    # noqa: BLE001
        return Result("youtube", ok=False, error=f"{type(e).__name__}: {e}")


# ── Kick ──────────────────────────────────────────────────────────────────
def fetch_kick(top_n: int) -> Result:
    """
    Kick 冇官方「全站排行」endpoint 可以免 key 用。
    做法: 試佢個公開 livestreams browse API; 俾 Cloudflare 擋就報錯跳過。
    """
    urls = [
        f"https://kick.com/api/v1/livestreams?limit={min(100, top_n)}",
        "https://kick.com/stream/livestreams/en",
    ]
    last_err = ""
    for url in urls:
        try:
            d = _get_json(url, headers={
                "Accept": "application/json",
                "Referer": "https://kick.com/",
            })
            rows = d.get("data") if isinstance(d, dict) else d
            if isinstance(rows, dict):
                rows = rows.get("livestreams") or rows.get("data") or []
            if not isinstance(rows, list) or not rows:
                last_err = "回應冇 livestream 資料"
                continue
            out = []
            for s in rows[:top_n]:
                ch = s.get("channel") or {}
                slug = (ch.get("slug") or s.get("slug")
                        or (s.get("user") or {}).get("username") or "")
                cat = ""
                cats = s.get("categories") or []
                if cats and isinstance(cats, list):
                    cat = (cats[0] or {}).get("name", "")
                out.append(Streamer(
                    platform="kick",
                    name=(ch.get("user") or {}).get("username") or slug,
                    viewers=int(s.get("viewer_count") or s.get("viewers") or 0),
                    title=s.get("session_title") or "",
                    category=cat,
                    url=f"https://kick.com/{slug}" if slug else "https://kick.com",
                    language=s.get("language") or "",
                    avatar=(s.get("thumbnail") or {}).get("url", "")
                    if isinstance(s.get("thumbnail"), dict)
                    else (s.get("thumbnail") or ""),
                    channel_id=slug,
                ))
            out = [s for s in out if s.viewers > 0]
            out.sort(key=lambda s: s.viewers, reverse=True)
            if out:
                _log("kick:", len(out))
                return Result("kick", streamers=out)
            last_err = "全部 viewer_count 都係 0"
        except Exception as e:                                # noqa: BLE001
            last_err = f"{type(e).__name__}: {e}"
    return Result("kick", ok=False,
                  error=f"{last_err} (Kick 由 datacenter IP 出去成日俾 Cloudflare 擋)")


# ── TikTok ────────────────────────────────────────────────────────────────
_TIKTOK_JSON_RE = re.compile(
    r'<script[^>]+id="(?:SIGI_STATE|__UNIVERSAL_DATA_FOR_REHYDRATION__)"[^>]*>(.*?)</script>',
    re.S)


def _deep_find_lives(node, out: list, depth: int = 0):
    """喺一嚿唔知結構嘅 JSON 入面, 遞迴搵似『直播房』嘅 record。"""
    if depth > 12 or len(out) > 400:
        return
    if isinstance(node, dict):
        vc = node.get("userCount", node.get("viewerCount", node.get("user_count")))
        nick = node.get("nickname") or node.get("uniqueId") or ""
        if isinstance(vc, (int, float)) and vc > 0 and nick:
            out.append(node)
        for v in node.values():
            _deep_find_lives(v, out, depth + 1)
    elif isinstance(node, list):
        for v in node:
            _deep_find_lives(v, out, depth + 1)


def fetch_tiktok(top_n: int) -> Result:
    """
    TikTok 冇官方 live API。抽 /live 頁面入面嘅 JSON state。
    由 GitHub Actions 出去好大機會攞唔到 (要 cookie / 地區), 攞唔到就照報錯跳過。
    """
    try:
        html = _request("https://www.tiktok.com/live", headers={
            "Accept": "text/html,application/xhtml+xml",
            "Accept-Language": "en-US,en;q=0.9",
        })
        blobs = _TIKTOK_JSON_RE.findall(html)
        if not blobs:
            return Result("tiktok", ok=False,
                          error="頁面冇 SIGI_STATE (好可能俾 TikTok 擋咗 / 要登入)")
        found = []
        for b in blobs:
            try:
                _deep_find_lives(json.loads(b), found)
            except Exception:                                 # noqa: BLE001
                continue
        seen, out = set(), []
        for r in found:
            nick = r.get("nickname") or r.get("uniqueId") or ""
            uid = r.get("uniqueId") or nick
            if not uid or uid in seen:
                continue
            seen.add(uid)
            out.append(Streamer(
                platform="tiktok",
                name=nick or uid,
                viewers=int(r.get("userCount") or r.get("viewerCount")
                            or r.get("user_count") or 0),
                title=r.get("title") or "",
                category="",
                url=f"https://www.tiktok.com/@{uid}/live",
                language="",
                avatar=r.get("avatarThumb") or r.get("avatarLarger") or "",
                channel_id=uid,
            ))
        out.sort(key=lambda s: s.viewers, reverse=True)
        out = out[:top_n]
        if not out:
            return Result("tiktok", ok=False, error="抽到 JSON 但搵唔到直播房")
        _log("tiktok:", len(out))
        return Result("tiktok", streamers=out)
    except Exception as e:                                    # noqa: BLE001
        return Result("tiktok", ok=False, error=f"{type(e).__name__}: {e}")


FETCHERS = {
    "twitch": fetch_twitch,
    "youtube": fetch_youtube,
    "kick": fetch_kick,
    "tiktok": fetch_tiktok,
}


def fetch_all(platforms=None, top_n: int = 60) -> list:
    """跑晒所有平台, 回傳 list[Result] (成功同失敗都會有)。"""
    results = []
    for p in (platforms or config.PLATFORMS):
        fn = FETCHERS.get(p)
        if not fn:
            results.append(Result(p, ok=False, error="唔認識呢個平台"))
            continue
        try:
            results.append(fn(top_n))
        except Exception as e:                                # noqa: BLE001
            results.append(Result(p, ok=False, error=f"{type(e).__name__}: {e}"))
    return results
