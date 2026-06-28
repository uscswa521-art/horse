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

    # 地點: 大班旅遊 Tai Pan Tours Head Office
    # 3621 Hwy 7 Suite 509, Markham, ON L3R 0G6 (Markham Town Centre 一帶)
    "city": os.environ.get("CITY", "Markham"),
    "office_lat": _float("OFFICE_LAT", 43.8545),
    "office_lng": _float("OFFICE_LNG", -79.3368),
    # 只接受步行 10-15 分鐘 (約 1.5km 直線距離) 以內。
    "radius_km": _float("RADIUS_KM", 1.5),
    # 步行範圍好窄, 所以一定要有座標先計到距離; 冇座標嘅盤直接唔要 (=1)。
    "require_coords": os.environ.get("REQUIRE_COORDS", "1").lower()
    not in ("0", "false", "no", ""),

    # 地點關鍵字 (淨係冇座標 + require_coords=0 嗰陣先用嚟粗略判斷)
    "location_keywords": [
        k.strip().lower()
        for k in os.environ.get(
            "LOCATION_KEYWORDS",
            "south town centre,town centre blvd,cedarland,clegg,"
            "enterprise blvd,downtown markham,riverlands,l3r 0g,l3r 9",
        ).split(",")
        if k.strip()
    ],

    # 唔要嘅盤 (地庫/basement 等), 個盤文字含到任何一個就剔走
    "exclude_keywords": [
        k.strip().lower()
        for k in os.environ.get(
            "EXCLUDE_KEYWORDS",
            "basement,bsmt,b/t,地庫,地下室,半地庫,lower level,"
            "walkout basement,walk-out basement,walkout bsmt",
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
    # chat_id: 已由實際運行偵測到並寫死, 確保長期穩定 (唔使靠 24 小時內有 message)。
    # 想改/保密可改用環境變數 TELEGRAM_CHAT_ID 覆蓋。
    "chat_id": os.environ.get("TELEGRAM_CHAT_ID", "278197406").strip(),
}

# 狀態檔 (記住邊啲盤匯報過, 避免重複)
STATE_FILE = os.environ.get(
    "STATE_FILE",
    os.path.join(os.path.dirname(__file__), "state", "seen.json"),
)
