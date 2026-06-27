"""
搵屋設定 / Property search configuration.

所有設定都可以用環境變數覆蓋 (GitHub Actions 用 secrets / env)。
All values can be overridden with environment variables.
"""
import os


def _int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def _float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


# ── 搜尋條件 / Search criteria ────────────────────────────────────────────
# 確認需求: 租樓、2 睡房 / 2 浴室、≤ $2,800/月、Markham 近大班旅遊
SEARCH = {
    # 租 (rent) 定 買 (buy)
    "deal_type": os.environ.get("DEAL_TYPE", "rent"),

    # 房 / 廁
    "bedrooms_min": _int("BEDROOMS_MIN", 2),
    "bedrooms_max": _int("BEDROOMS_MAX", 2),
    "bathrooms_min": _int("BATHROOMS_MIN", 2),

    # 租金上限 (CAD / 月)
    "price_max": _int("PRICE_MAX", 2800),
    "price_min": _int("PRICE_MIN", 0),

    # 地點: 大班旅遊 Tai Pan Tours Head Office, Markham ON
    # (South Town Centre Blvd / Clegg Rd 一帶, 近 Markham Town Square / Hwy 404)
    "city": os.environ.get("CITY", "Markham"),
    "office_lat": _float("OFFICE_LAT", 43.8561),
    "office_lng": _float("OFFICE_LNG", -79.3370),
    # 距離公司幾遠以內先算符合 (km)。如果筍盤冇座標就唔會用呢個過濾。
    "radius_km": _float("RADIUS_KM", 12.0),

    # 地點關鍵字 (如果筍盤冇座標, 用嚟粗略判斷係咪喺附近)
    "location_keywords": [
        k.strip().lower()
        for k in os.environ.get(
            "LOCATION_KEYWORDS",
            "markham,unionville,south town centre,cedarland,clegg,"
            "warden,enterprise,downtown markham,markham town square,"
            "l3r,l6g,l6c",
        ).split(",")
        if k.strip()
    ],
}

# ── Telegram ──────────────────────────────────────────────────────────────
# ⚠️ 唔好將 token 寫死喺 code 入面 / NEVER hardcode the token.
#    GitHub Actions: 用 repo secret  TELEGRAM_BOT_TOKEN
TELEGRAM = {
    # .strip() 清走貼 token 時可能多咗嘅換行/空白 (control char 會整壞網址)
    "bot_token": os.environ.get("TELEGRAM_BOT_TOKEN", "").strip(),
    # 留空就會自動用 getUpdates 搵返你嘅 chat id (你只要同 bot 講過一句嘢)
    "chat_id": os.environ.get("TELEGRAM_CHAT_ID", "").strip(),
}

# 狀態檔 (記住邊啲盤匯報過, 避免重複)
STATE_FILE = os.environ.get(
    "STATE_FILE",
    os.path.join(os.path.dirname(__file__), "state", "seen.json"),
)
