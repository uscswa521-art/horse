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
import os
import re
import urllib.request
from html import unescape
from dataclasses import dataclass, field
from math import asin, cos, radians, sin, sqrt
from typing import Iterable, Optional

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"
)

# 用唔用 Scrapling 嘅隱身瀏覽器 (StealthyFetcher) 去破反爬蟲。
# GitHub Actions 預設開 (USE_STEALTH=1)。設 0 就用普通 urllib。
USE_STEALTH = os.environ.get("USE_STEALTH", "1").lower() not in ("0", "false", "no", "")
DEBUG = bool(os.environ.get("DEBUG"))
# 設咗就會將每個網 rendered HTML 寫落呢個資料夾 (調試/寫 parser 用)
DUMP_DIR = os.environ.get("DUMP_DIR")


def _maybe_dump(name: str, html: str):
    if not DUMP_DIR:
        return
    try:
        os.makedirs(DUMP_DIR, exist_ok=True)
        with open(os.path.join(DUMP_DIR, f"{name}.html"), "w", encoding="utf-8") as f:
            f.write(html)
    except Exception:  # noqa: BLE001
        pass


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
def _fetch_urllib(url: str, timeout: int = 30, headers: dict = None) -> str:
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


def _fetch_stealth(url: str, timeout: int = 60) -> str:
    """用 Scrapling 隱身瀏覽器攞 rendered HTML, 會自動試破 Cloudflare。"""
    from scrapling.fetchers import StealthyFetcher  # 延遲 import, 冇裝都唔會即炸
    page = StealthyFetcher.fetch(
        url,
        headless=True,
        network_idle=True,
        solve_cloudflare=True,
        google_search=False,
        timeout=timeout * 1000,
    )
    html = getattr(page, "html_content", None) or getattr(page, "body", "") or ""
    if DEBUG:
        print(f"[stealth] {url} status={getattr(page, 'status', '?')} "
              f"htmllen={len(html)}")
    return html


def fetch(url: str, timeout: int = 30, headers: dict = None) -> str:
    """預設行隱身瀏覽器; 失敗就回落普通 urllib。"""
    if USE_STEALTH:
        try:
            return _fetch_stealth(url, max(timeout, 60))
        except Exception as e:  # noqa: BLE001
            if DEBUG:
                print(f"[stealth] {url} 失敗, 回落 urllib: {type(e).__name__}: {e}")
    return _fetch_urllib(url, timeout, headers)


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
    # API 經瀏覽器返嚟時, JSON 可能淨係包喺 <pre> 入面, 或者成個 body 就係 JSON
    for m in re.finditer(r"<pre[^>]*>(.*?)</pre>", html, re.DOTALL | re.IGNORECASE):
        try:
            yield json.loads(unescape(m.group(1)).strip())
        except (json.JSONDecodeError, ValueError):
            continue
    stripped = html.strip()
    if stripped[:1] in "{[":
        try:
            yield json.loads(stripped)
        except (json.JSONDecodeError, ValueError):
            pass


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
# 用隱身瀏覽器(StealthyFetcher)攞 rendered 頁面, 再用通用 JSON 抽取器搵筍盤。
# 加新網站只要喺 SOURCES 加多一個 function。
def _page_source(name: str, url: str, base: str) -> list:
    html = fetch(url)
    _maybe_dump(name, html)
    listings = extract_listings(html, name, base)
    if DEBUG:
        marks = {k: html.count(k) for k in
                 ('__NEXT_DATA__', '__next_f', 'application/ld+json',
                  '"price"', '"bedrooms"', '"numberOfRooms"')}
        print(f"[{name}] htmllen={len(html)} 抽到={len(listings)} marks={marks}")
    return listings


def src_rentfaster(cfg) -> list:
    """RentFaster.ca — 有公開 JSON API。先試 API(經隱身瀏覽器), 唔得就試城市頁。"""
    base = "https://www.rentfaster.ca"
    params = (
        "keywords=Markham"
        f"&beds={int(cfg['bedrooms_min'])}"
        f"&price_range_adv%5Bfrom%5D={int(cfg['price_min'])}"
        f"&price_range_adv%5Bto%5D={int(cfg['price_max'])}"
        "&novalified=1&page=1"
    )
    html = fetch(f"{base}/api/search.php?{params}")
    listings = []
    for blob in _iter_json_blobs(html):
        arr = blob.get("listings") if isinstance(blob, dict) else None
        for it in (arr or []):
            if not isinstance(it, dict):
                continue
            link = it.get("link") or it.get("url") or ""
            listings.append(Listing(
                source="rentfaster.ca",
                title=str(it.get("title") or it.get("type") or "RentFaster 盤")[:200],
                price=_num(it.get("price")),
                bedrooms=_num(it.get("bedrooms") or it.get("Bedrooms")),
                bathrooms=_num(it.get("bathrooms") or it.get("Bathrooms")
                               or it.get("baths")),
                address=str(it.get("address") or it.get("location")
                            or it.get("city") or "")[:250],
                url=(base + str(link)) if str(link).startswith("/") else str(link),
                lat=_num(it.get("latitude")),
                lng=_num(it.get("longitude")),
                raw_keywords=json.dumps(it, ensure_ascii=False)[:500],
            ))
    if not listings:  # 回落: rendered 城市頁
        listings = _page_source("rentfaster.ca", f"{base}/on/markham/rentals/", base)
    if DEBUG:
        print(f"[rentfaster.ca] api_htmllen={len(html)} 抽到={len(listings)}")
    return listings


def src_rentals_ca(cfg) -> list:
    url = (f"https://rentals.ca/markham?beds={cfg['bedrooms_min']}"
           f"&baths={cfg['bathrooms_min']}&p_h={cfg['price_max']}")
    return _page_source("rentals.ca", url, "https://rentals.ca")


def src_kijiji(cfg) -> list:
    url = ("https://www.kijiji.ca/b-for-rent/markham/"
           f"page-1/c30349001l1700274?ad=offering&price=__{cfg['price_max']}")
    return _page_source("kijiji.ca", url, "https://www.kijiji.ca")


def src_zumper(cfg) -> list:
    return _page_source("zumper.com",
                        "https://www.zumper.com/apartments-for-rent/markham-on",
                        "https://www.zumper.com")


def src_padmapper(cfg) -> list:
    return _page_source("padmapper.com",
                        "https://www.padmapper.com/apartments/markham-on",
                        "https://www.padmapper.com")


SOURCES = [
    ("rentfaster.ca", src_rentfaster),
    ("rentals.ca", src_rentals_ca),
    ("kijiji.ca", src_kijiji),
    ("zumper.com", src_zumper),
    ("padmapper.com", src_padmapper),
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
