"""
搵屋爬蟲 / Property scrapers.

策略: 唔靠某個網站固定嘅 HTML class (佢哋成日改版),
而係由頁面抽出所有 JSON 區塊 (__NEXT_DATA__ / application+ld/json / window.__X=)
再用一個通用嘅遞迴抽取器搵出「似筍盤」嘅 record。
咁樣對網站改版好有抵抗力。

每個來源都會獨立 try/except, 一個塌咗唔會影響其他。
注意: 由 datacenter IP (例如 GitHub Actions) 出去, 部分網站可能被 Cloudflare
擋。如果長期攞唔到資料, 可考慮加 residential proxy (見 README)。
"""
import json
import re
import urllib.request
from dataclasses import dataclass, field
from math import asin, cos, radians, sin, sqrt
from typing import Iterable, Optional

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)


# ── 資料模型 / Data model ──────────────────────────────────────────────────
@dataclass
class Listing:
    source: str
    title: str = ""
    price: Optional[float] = None
    bedrooms: Optional[float] = None
    bathrooms: Optional[float] = None
    address: str = ""
    url: str = ""
    lat: Optional[float] = None
    lng: Optional[float] = None
    raw_keywords: str = field(default="", repr=False)

    def uid(self) -> str:
        """穩定 ID, 用嚟 dedupe。"""
        if self.url:
            return f"{self.source}:{self.url.split('?')[0]}"
        return f"{self.source}:{self.title}:{self.address}:{self.price}"

    def text_blob(self) -> str:
        return " ".join(
            str(x).lower()
            for x in (self.title, self.address, self.url, self.raw_keywords)
            if x
        )


# ── HTTP ────────────────────────────────────────────────────────────────
def fetch(url: str, timeout: int = 30, headers: dict = None) -> str:
    h = {
        "User-Agent": UA,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "en-CA,en;q=0.9,zh-HK;q=0.8",
    }
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        charset = resp.headers.get_content_charset() or "utf-8"
        return resp.read().decode(charset, errors="replace")


# ── JSON 區塊抽取 / Extract JSON blobs from HTML ──────────────────────────
_JSON_SCRIPT_RE = re.compile(
    r'<script[^>]*type=["\']application/(?:ld\+json|json)["\'][^>]*>(.*?)</script>',
    re.DOTALL | re.IGNORECASE,
)
_NEXT_RE = re.compile(
    r'<script[^>]*id=["\']__NEXT_DATA__["\'][^>]*>(.*?)</script>',
    re.DOTALL | re.IGNORECASE,
)
_WINDOW_RE = re.compile(
    r'window\.__[A-Z_]+__\s*=\s*(\{.*?\})\s*;?\s*</script>',
    re.DOTALL,
)


def _iter_json_blobs(html: str) -> Iterable[dict]:
    for rx in (_NEXT_RE, _JSON_SCRIPT_RE, _WINDOW_RE):
        for m in rx.finditer(html):
            txt = m.group(1).strip()
            if not txt:
                continue
            try:
                yield json.loads(txt)
            except (json.JSONDecodeError, ValueError):
                continue


# ── 數字 normalise ─────────────────────────────────────────────────────────
def _num(val) -> Optional[float]:
    if val is None:
        return None
    if isinstance(val, (int, float)):
        return float(val)
    m = re.search(r"-?\d[\d,]*\.?\d*", str(val).replace(",", ""))
    return float(m.group()) if m else None


_PRICE_KEYS = ("price", "rent", "askingprice", "monthlyrent", "amount", "value")
_BED_KEYS = ("bedrooms", "beds", "numberofrooms", "bedroom", "bed")
_BATH_KEYS = ("bathrooms", "baths", "bathroom", "bath", "numberofbathroomstotal")
_ADDR_KEYS = ("address", "streetaddress", "formattedaddress", "location", "fulladdress")
_URL_KEYS = ("url", "link", "detailurl", "permalink", "seourl", "canonicalurl")
_TITLE_KEYS = ("title", "name", "heading", "description")


def _get(d: dict, keys) -> Optional[object]:
    low = {k.lower(): v for k, v in d.items() if isinstance(k, str)}
    for k in keys:
        if k in low and low[k] not in (None, "", []):
            return low[k]
    return None


def _flatten_addr(val) -> str:
    if isinstance(val, str):
        return val
    if isinstance(val, dict):
        parts = [
            str(val.get(k))
            for k in ("streetAddress", "addressLocality", "addressRegion",
                      "postalCode", "city", "province", "street")
            if val.get(k)
        ]
        return ", ".join(parts)
    return ""


def _looks_like_listing(d: dict) -> bool:
    if not isinstance(d, dict):
        return False
    has_price = _get(d, _PRICE_KEYS) is not None
    has_room = _get(d, _BED_KEYS) is not None or _get(d, _BATH_KEYS) is not None
    has_id = _get(d, _ADDR_KEYS) is not None or _get(d, _URL_KEYS) is not None \
        or _get(d, _TITLE_KEYS) is not None
    return has_price and has_room and has_id


def _walk(obj, out: list, depth: int = 0):
    if depth > 12:
        return
    if isinstance(obj, dict):
        if _looks_like_listing(obj):
            out.append(obj)
        for v in obj.values():
            _walk(v, out, depth + 1)
    elif isinstance(obj, list):
        for v in obj:
            _walk(v, out, depth + 1)


def _coerce(d: dict, source: str, base_url: str = "") -> Optional[Listing]:
    url = _get(d, _URL_KEYS)
    url = str(url) if url else ""
    if url and url.startswith("/") and base_url:
        url = base_url.rstrip("/") + url
    addr = _flatten_addr(_get(d, _ADDR_KEYS))
    title = _get(d, _TITLE_KEYS)
    geo = d.get("geo") if isinstance(d.get("geo"), dict) else {}
    lat = _num(d.get("latitude") or geo.get("latitude") or d.get("lat"))
    lng = _num(d.get("longitude") or geo.get("longitude") or d.get("lng")
               or d.get("lon"))
    lst = Listing(
        source=source,
        title=str(title or "")[:200],
        price=_num(_get(d, _PRICE_KEYS)),
        bedrooms=_num(_get(d, _BED_KEYS)),
        bathrooms=_num(_get(d, _BATH_KEYS)),
        address=addr[:250],
        url=url,
        lat=lat,
        lng=lng,
        raw_keywords=json.dumps(d, ensure_ascii=False)[:500],
    )
    if lst.price is None and lst.bedrooms is None:
        return None
    return lst


def extract_listings(html: str, source: str, base_url: str = "") -> list:
    found, seen_uids, results = [], set(), []
    for blob in _iter_json_blobs(html):
        _walk(blob, found)
    for d in found:
        lst = _coerce(d, source, base_url)
        if lst and lst.uid() not in seen_uids:
            seen_uids.add(lst.uid())
            results.append(lst)
    return results


# ── 各個來源 / Sources ─────────────────────────────────────────────────────
# 每個來源 = (名, function -> [Listing])。加新網站只要加多個 function。
def src_rentals_ca(cfg) -> list:
    city = cfg["city"].lower().replace(" ", "-")
    url = (f"https://rentals.ca/{city}?beds={cfg['bedrooms_min']}"
           f"&baths={cfg['bathrooms_min']}&p_h={cfg['price_max']}")
    html = fetch(url)
    return extract_listings(html, "rentals.ca", "https://rentals.ca")


def src_rentfaster(cfg) -> list:
    """
    RentFaster.ca — 有公開 JSON API, 唔使瀏覽器, 最有機會由 datacenter IP 攞到嘢。
    注意: RentFaster 喺西岸(AB/BC)盤多, 安大略(Markham)盤可能比較少。
    """
    base = "https://www.rentfaster.ca"
    headers = {
        "Referer": f"{base}/on/markham/rentals/",
        "X-Requested-With": "XMLHttpRequest",
        "Accept": "application/json, text/javascript, */*; q=0.01",
    }
    params = (
        "keywords=Markham"
        f"&beds={int(cfg['bedrooms_min'])}"
        f"&price_range_adv%5Bfrom%5D={int(cfg['price_min'])}"
        f"&price_range_adv%5Bto%5D={int(cfg['price_max'])}"
        "&novalified=1&page=1"
    )
    raw = fetch(f"{base}/api/search.php?{params}", headers=headers)
    try:
        data = json.loads(raw)
    except (json.JSONDecodeError, ValueError):
        # 攞到 HTML (多數係 Cloudflare 攔截頁) 而唔係 JSON
        raise RuntimeError("rentfaster 回傳唔係 JSON (可能被攔截)")
    out = []
    for it in (data.get("listings") or []):
        if not isinstance(it, dict):
            continue
        link = it.get("link") or it.get("url") or ""
        out.append(Listing(
            source="rentfaster.ca",
            title=str(it.get("title") or it.get("type") or "RentFaster 盤")[:200],
            price=_num(it.get("price")),
            bedrooms=_num(it.get("bedrooms") or it.get("Bedrooms")),
            bathrooms=_num(it.get("bathrooms") or it.get("Bathrooms") or it.get("baths")),
            address=str(it.get("address") or it.get("location") or it.get("city") or "")[:250],
            url=(base + str(link)) if str(link).startswith("/") else str(link),
            lat=_num(it.get("latitude")),
            lng=_num(it.get("longitude")),
            raw_keywords=json.dumps(it, ensure_ascii=False)[:500],
        ))
    return out


def src_kijiji(cfg) -> list:
    # Kijiji apartments/condos for rent in Markham
    url = ("https://www.kijiji.ca/b-for-rent/markham/"
           f"page-1/c30349001l1700274?ad=offering&price=__{cfg['price_max']}")
    html = fetch(url)
    return extract_listings(html, "kijiji.ca", "https://www.kijiji.ca")


def src_zumper(cfg) -> list:
    city = cfg["city"].lower().replace(" ", "-")
    url = f"https://www.zumper.com/apartments-for-rent/{city}-on"
    html = fetch(url)
    return extract_listings(html, "zumper.com", "https://www.zumper.com")


def src_padmapper(cfg) -> list:
    city = cfg["city"].lower().replace(" ", "-")
    url = f"https://www.padmapper.com/apartments/{city}-on"
    html = fetch(url)
    return extract_listings(html, "padmapper.com", "https://www.padmapper.com")


# 排先嘅最有機會由 datacenter IP 攞到資料。
# rentals.ca / kijiji / zumper / padmapper 經實測由雲端伺服器多數被反爬蟲擋 (403/0),
# 留住佢哋只係「有殺錯冇放過」, 主力係 rentfaster。
SOURCES = [
    ("rentfaster.ca", src_rentfaster),
    ("kijiji.ca", src_kijiji),
    ("zumper.com", src_zumper),
]


# ── 距離 / Distance (haversine, km) ───────────────────────────────────────
def distance_km(lat1, lng1, lat2, lng2) -> Optional[float]:
    if None in (lat1, lng1, lat2, lng2):
        return None
    r = 6371.0
    dlat, dlng = radians(lat2 - lat1), radians(lng2 - lng1)
    a = (sin(dlat / 2) ** 2
         + cos(radians(lat1)) * cos(radians(lat2)) * sin(dlng / 2) ** 2)
    return 2 * r * asin(sqrt(a))
