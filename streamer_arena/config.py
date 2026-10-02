"""
Streamer Arena 設定 / configuration.

所有嘢都可以用環境變數覆蓋 (GitHub Actions 用 secrets / env)。
一個 key 都冇都照跑得 — 冇 key 嘅平台會自動跳過, 全部平台都失敗就出 demo 數據,
而且會喺 JSON 入面誠實標明 source = "demo"。
"""
import os


def _int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def _bool(name: str, default: bool = False) -> bool:
    v = os.environ.get(name)
    if v is None:
        return default
    return v.strip().lower() in ("1", "true", "yes", "on")


def _list(name: str, default: str) -> list:
    return [x.strip() for x in os.environ.get(name, default).split(",") if x.strip()]


# ── 出邊個平台 / Which platforms ──────────────────────────────────────────
PLATFORMS = _list("PLATFORMS", "twitch,youtube,kick,tiktok")

# 每個平台攞幾多個直播主 (由多人睇排落去)
TOP_N = _int("TOP_N", 60)

# 寫去邊 (arena.html 會 fetch 呢個檔)
OUT_FILE = os.environ.get(
    "OUT_FILE",
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                 "data", "streamers.json"),
)

# 留低歷史 (畀 arena.html 畫「人潮走勢」), 保留最近幾多個 snapshot
HISTORY_FILE = os.environ.get(
    "HISTORY_FILE",
    os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                 "data", "history.json"),
)
HISTORY_KEEP = _int("HISTORY_KEEP", 288)   # 288 × 20 分鐘 ≈ 4 日

HTTP_TIMEOUT = _int("HTTP_TIMEOUT", 20)
DEBUG = _bool("DEBUG", False)

# ── Twitch ────────────────────────────────────────────────────────────────
# https://dev.twitch.tv/console/apps → 開一個 app 攞 Client ID + Secret
# GitHub secrets: TWITCH_CLIENT_ID / TWITCH_CLIENT_SECRET
TWITCH = {
    "client_id": os.environ.get("TWITCH_CLIENT_ID", ""),
    "client_secret": os.environ.get("TWITCH_CLIENT_SECRET", ""),
}

# ── YouTube ───────────────────────────────────────────────────────────────
# https://console.cloud.google.com → YouTube Data API v3 → API key
# ⚠️ quota 一日 10,000 units, 一次 search = 100 units。
#    預設每 20 分鐘跑一次 = 72 次/日 × (100 + 1) ≈ 7,300 units, 岩岩夠。
#    跑密過呢個就會 quota exceeded, 所以唔好將 cron 改到太密。
YOUTUBE = {
    "api_key": os.environ.get("YOUTUBE_API_KEY", ""),
    # 搜咩 (留空 = 攞全球最多人睇嘅 live)
    "query": os.environ.get("YOUTUBE_QUERY", ""),
    "region": os.environ.get("YOUTUBE_REGION", ""),   # e.g. HK / US, 留空 = 全球
}

# ── Kick ──────────────────────────────────────────────────────────────────
# Kick 有公開 API, 唔使 key, 但係由 datacenter IP (GitHub Actions) 出去
# 好大機會俾 Cloudflare 擋。擋咗就自動跳過, 唔會拖冧其他平台。
KICK = {
    # 官方 OAuth (有就用, 冇就試公開 endpoint)
    "client_id": os.environ.get("KICK_CLIENT_ID", ""),
    "client_secret": os.environ.get("KICK_CLIENT_SECRET", ""),
}

# ── TikTok ────────────────────────────────────────────────────────────────
# TikTok 冇官方 live API。只可以 scrape /live 頁面入面嘅 JSON,
# 成功率視乎 IP 同埋佢改唔改版 — 攞唔到就照跳過。
TIKTOK = {
    # 想穩陣可以自己指定要跟開邊幾個 (用 @username, 逗號分隔)
    "channels": _list("TIKTOK_CHANNELS", ""),
}
