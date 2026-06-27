# 🏠 搵屋爬蟲 → Telegram (Foundhorse_bot)

自動掃描租屋網, 搵 **近大班旅遊 (Tai Pan Tours, Markham ON)** 嘅租盤,
符合就即刻 Telegram 通知你, **每 1 小時匯報一次 (即使冇新盤都會講)**。

## 搜尋條件 (你確認咗嘅)
| 項目 | 設定 |
|---|---|
| 租 / 買 | **租** |
| 間隔 | **2 房 2 廁** |
| 租金 | **≤ $2,800 / 月 (CAD)** |
| 地點 | **大班旅遊附近** (Markham, 近 Downtown Markham / Markham Town Square / Hwy 404, 預設 12km 內) |
| 匯報 | **每 1 小時, 無論有冇都報** |

全部都可以改 — 見下面「設定」。

---

## ⚠️ 重要: 先換 Telegram token
你個 token 喺截圖入面已經 **公開咗**, 任何人見到都可以控制你個 bot。
請喺 Telegram 搵 **@BotFather → `/revoke` → 揀 Foundhorse_bot** 攞一個新 token,
然後將 **新** token 放入 GitHub secret (下面教)。
👉 我**冇**將 token 寫入任何 code / 檔案, 佢只會由 GitHub secret 讀取。

---

## 設定 (GitHub Actions — 免費雲端, 唔使開電腦)

### 1. 加 Secret
GitHub repo → **Settings → Secrets and variables → Actions → New repository secret**

| Name | Value |
|---|---|
| `TELEGRAM_BOT_TOKEN` | 你 BotFather 攞嘅 (新) token |
| `TELEGRAM_CHAT_ID` | *(可選)* 你嘅 chat id；留空程式會自動偵測 |

### 2. 同個 bot 講一句嘢
喺 Telegram 開 **@Foundhorse_bot → 㩒 Start → 隨便打句嘢**。
咁程式先至搵到要send畀邊個 (chat id)。

### 3. 開 Actions
repo 嘅 **Actions** 分頁 → 如果問你 enable workflows 就㩒 enable。
之後想即刻試: 揀 **搵屋爬蟲 / Property Finder → Run workflow**。
之後佢會自己 **每個鐘** 跑一次。

> Workflow 檔: `.github/workflows/property-finder.yml`（cron `0 * * * *`）。
> 每次跑完會將「已匯報嘅盤」記入 `state/seen.json` 並 commit 返, 所以同一個盤
> 唔會 send 兩次。

---

## 喺自己電腦跑 (另一個選擇)
```bash
export TELEGRAM_BOT_TOKEN="你嘅token"
# export TELEGRAM_CHAT_ID="你嘅chatid"   # 可選

python3 -m property_finder.finder            # 跑一次
python3 -m property_finder.finder --loop     # 長駐, 每小時一次 (要開住部機)
```
冇第三方套件, 只用 Python 3.9+ 標準庫。

---

## 設定 (環境變數覆蓋, 唔使改 code)
| 變數 | 預設 | 意思 |
|---|---|---|
| `PRICE_MAX` | `2800` | 租金上限 |
| `BEDROOMS_MIN` / `BEDROOMS_MAX` | `2` / `2` | 房數 |
| `BATHROOMS_MIN` | `2` | 廁數 |
| `CITY` | `Markham` | 城市 |
| `OFFICE_LAT` / `OFFICE_LNG` | `43.8561` / `-79.3370` | 公司座標 |
| `RADIUS_KM` | `12` | 公司幾多 km 內 |
| `LOCATION_KEYWORDS` | markham,unionville,… | 冇座標時用嘅地點關鍵字 |

---

## 掃緊邊啲網
`rentals.ca`、`kijiji.ca`、`zumper.com`、`padmapper.com`。
加新網好簡單: 喺 `scrapers.py` 嘅 `SOURCES` 加多個 function 就得。

爬蟲唔靠固定 HTML class (網站成日改版), 而係抽頁面入面嘅 JSON
再用通用邏輯認出「似租盤」嘅資料 → 對改版有抵抗力。

### ⚠️ 限制 (老實講)
由 **datacenter IP (GitHub Actions)** 出去, 部分網站 (Kijiji 等) 可能被
Cloudflare 擋, 攞唔到資料 — 呢個係正常現象, 唔係程式壞咗。匯報會照出, 並
喺「已掃 / 出錯」一行話你知邊個來源成功、邊個失敗。如果長期某個來源都失敗,
可以:
1. 改去自己電腦 / 屋企網絡跑 (`--loop`), 通常冇咁易被擋；或
2. 加 residential proxy (設 `HTTPS_PROXY` 環境變數)。

---

## 檔案
```
property_finder/
├─ finder.py          # 主程式: 掃 → 過濾 → 推送 → 匯報
├─ scrapers.py        # 各網站爬蟲 + 通用 JSON 抽取 + 距離計算
├─ telegram_notify.py # Telegram 發送 + 自動偵測 chat id
├─ config.py          # 搜尋條件 (可用環境變數覆蓋)
└─ state/seen.json    # 記住已匯報嘅盤 (自動更新)
.github/workflows/property-finder.yml  # 每小時 cron
```
