# 🎤 Streamer Arena — 全球直播人潮「巨像化」

將全世界直播主嘅**即時觀看人數**，變成一個**你真係行得入去嘅 3D 演唱會場館**：

> **一個公仔 = 一個真人。**
> 觀眾企喺碗形看台度郁、擰身、揈燈棒，人浪一圈圈由舞台掃出去；
> 直播主喺台上行嚟行去。你可以拖鏡頭、由高空俯瞰成個場，
> 一路 zoom 落去直至企咗喺人堆中間 —— 就係入到紅館嗰種震撼。

再加**真實場館比例尺**（伊館 / 紅館 / 大球場 / 東京巨蛋 / 溫布萊 / 鳥巢）：
場入面會有一堵**金色光牆**圍住「一個紅館」嘅範圍，
你即刻見到成片人海裡面，一個紅館原來得咁細 —— 而家係 **80 個紅館**。

```
arena.html          ← 個 widget 本身 (HUD + 資料層)
arena3d.js          ← 3D 場館渲染器 (原生 WebGL2, 零外部依賴, 唔使 three.js)
streamer_arena/     ← 收數程式 (Twitch / YouTube / Kick / TikTok)
data/streamers.json ← GitHub Action 每 20 分鐘更新嘅 snapshot
```

## 點郁

| 操作 | 做咩 |
|---|---|
| **拖** | 轉視角（繞住場館轉） |
| **滾輪 / 兩指開合** | 由高空俯瞰 ↔ 企入人堆中間 |
| **WASD / 方向鍵** | 喺場館入面行過去 |
| **R** | 返回入場位 |
| **← →** | 換直播主 |
| **空白鍵** | 開／關自動巡場 |

冇人郁咗 7 秒，鏡頭會自己慢慢遊場（掛住做 OBS 背景都好睇）。

> **關於「幾多個公仔」**：一個真人一個公仔係目標，但係一百萬人畫唔晒。
> 畫面左邊會**誠實寫住**「1 個公仔 = 幾多人 / 場入面企緊幾多個」。
> 程式仲會**自動測幀數**：部機跑唔郁就少畫幾成人（均勻抽樣，個場唔會縮水），
> 夠力就自己加返上去，嗰行字亦都會即時跟住變。

---

## 三個模式

| 模式 | 用嚟做咩 | 網址 |
|---|---|---|
| **arena**（預設） | 全螢幕場館。自動巡場，一個一個直播主行過去 | `arena.html` |
| **overlay** | **OBS Browser Source**：透明底一條橫幅，自己個台 + 場館換算 | `arena.html?mode=overlay&me=twitch:你個channel` |
| **wall** | 全球直播主牆，每格一個迷你人海，面積同人數成正比 | `arena.html?mode=wall` |

### 放落 OBS
1. 加一個 **Browser Source**
2. URL 填：
   `https://<你個 GitHub Pages 網址>/arena.html?mode=overlay&me=twitch:你個channel`
3. 闊 × 高：**1000 × 190**（自己再拉都得）
4. ✅ 剔「Shutdown source when not visible」

底色自動透明（`mode=overlay` 預設 `bg=transparent`），OBS 見到嘅只有塊卡同人海。

### 嵌入自己個網站
```html
<iframe src="https://<你個網址>/arena.html?mode=overlay&me=kick:你個channel"
        width="1000" height="190" style="border:0;background:transparent"
        loading="lazy" title="直播人潮"></iframe>
```

---

## URL 參數

| 參數 | 預設 | 講解 |
|---|---|---|
| `mode` | `arena` | `arena` / `overlay` / `wall` |
| `me` | — | 鎖定邊個台。`twitch:xqc`、`kick:某人`，或者淨係打個 channel 名 |
| `bg` | `dark` | `transparent` = 透明底（overlay 模式自動用呢個） |
| `platform` | 全部 | 只睇某幾個平台，例如 `platform=twitch,kick` |
| `top` | `40` | 榜上排幾多個 |
| `unit` | `紅館` | 用邊個場館做換算單位（`伊館`/`紅館`/`亞博`/`大球場`/`東京巨蛋`/`溫布萊`/`鳥巢`） |
| `cycle` | `1` | `0` = 唔好自動巡場 |
| `cycleSec` | `9` | 幾多秒轉一個台 |
| `view` | `3d`（overlay 係 `2d`） | `3d` = 真三維場館；`2d` = 平面人海（慳電、老機、OBS 用） |
| `cam` | `auto` | `free` = 唔好自動遊場，淨係你自己控制 |
| `dots` | 3D `45000` / 2D `26000` | 場入面最多企幾多個公仔（跑唔郁會自動調低，畫面會寫返出嚟） |
| `refresh` | `60` | 幾多秒重新讀一次 JSON |
| `data` | `data/streamers.json` | 自己餵第二份數據都得 |

鍵盤：見上面「點郁」。

**點解 OBS overlay 預設用 2D？** 條橫幅得 190px 高，睇唔出立體；
而且 3D 會食直播主部機嘅 GPU（你打緊機同時 encode 緊）。
真係想要就加 `&view=3d`。

---

## 數據點嚟

`streamer_arena/collect.py` 同時問四個平台攞「而家有幾多人睇緊」，
**每個平台獨立 try/except — 一個死咗唔會拖冧其他**，
攞唔到嘅平台會照寫入 JSON 嘅 `platforms[x].error`，畫面下面會紅字寫返出嚟。
唔會靜雞雞當冇事發生。

| 平台 | 要唔要 key | 靠唔靠得住 |
|---|---|---|
| **Twitch** | 要（免費） | ⭐⭐⭐⭐⭐ 官方 Helix API，最齊最準 |
| **YouTube** | 要（免費，有 quota） | ⭐⭐⭐⭐ 官方 Data API v3，一日 10,000 units |
| **Kick** | 唔使 | ⭐⭐ 公開 endpoint，由 GitHub Actions 出去成日俾 Cloudflare 擋 |
| **TikTok** | 唔使 | ⭐ **冇官方 live API**，靠抽 `/live` 頁面嘅 JSON，改版／擋 IP 就攞唔到 |

> 講白啲：**Twitch 同 YouTube 加咗 key 就穩陣，Kick / TikTok 係 best-effort。**
> 一個平台都攞唔到嘅時候，程式會出**示範數據**（假數）等你至少睇到個效果，
> 但 JSON 會寫 `source: "demo"`，畫面亦會打「⚠️ 示範數據 · 未接 API key」水印。

### 加 API key（GitHub → Settings → Secrets and variables → Actions）

| Secret | 邊度攞 |
|---|---|
| `TWITCH_CLIENT_ID` | https://dev.twitch.tv/console/apps → Register Your Application |
| `TWITCH_CLIENT_SECRET` | 同上，撳 New Secret |
| `YOUTUBE_API_KEY` | https://console.cloud.google.com → 開 YouTube Data API v3 → 建立 API key |

加完之後去 **Actions → 🎤 直播人潮 → Run workflow** 撳一下，
`data/streamers.json` 就會由 `demo` 變 `live`。

⚠️ **唔好將 cron 改到密過 20 分鐘** — YouTube 一日 10,000 quota，
每次 search 食 100 units，改成 10 分鐘就會中午開始 403 quota exceeded。

---

## 自己喺電腦度跑

```bash
# 1. 整份示範數據 (唔使出街, 即刻睇到個效果)
python3 -m streamer_arena.collect --demo

# 2. 有 key 就攞真數據
export TWITCH_CLIENT_ID=xxx TWITCH_CLIENT_SECRET=yyy YOUTUBE_API_KEY=zzz
python3 -m streamer_arena.collect

# 3. 開個 http server 睇 (⚠️ 唔可以 file:// 直接開, fetch 會俾瀏覽器擋)
python3 -m http.server 8899
# 跟住開 http://localhost:8899/arena.html
```

其他選項：`--top 100`、`--platforms twitch,youtube`、`--out 邊度.json`、`--no-history`。

---

## JSON 格式

```jsonc
{
  "generated_at": "2026-07-30T22:58:45Z",
  "source": "live",              // live = 真數據 / mixed = 部分平台 / demo = 示範
  "total_viewers": 996653,
  "counted_streamers": 45,
  "platforms_ok": ["twitch", "youtube"],
  "platforms": {
    "twitch": { "ok": true, "error": "", "streamers": 60, "viewers": 299069, "top": "…" },
    "tiktok": { "ok": false, "error": "頁面冇 SIGI_STATE …", "streamers": 0, "viewers": 0 }
  },
  "streamers": [
    { "id": "twitch:xxx", "platform": "twitch", "name": "…", "viewers": 52341,
      "title": "…", "category": "…", "url": "…", "avatar": "…", "channel_id": "xxx" }
  ]
}
```

`data/history.json` 另外留低最近 288 個 snapshot（≈ 4 日）嘅總人數走勢。

---

## 場館容量（用嚟做比較嘅真實數字）

| 場館 | 容量 |
|---|---|
| 伊利沙伯體育館 | 3,500 |
| 紅磡體育館（紅館） | 12,500 |
| 亞洲國際博覽館 Arena | 14,000 |
| 香港大球場 | 40,000 |
| 東京巨蛋 | 55,000 |
| 溫布萊球場 | 90,000 |
| 北京鳥巢 | 91,000 |

（都係約數 / 演唱會設定下嘅常見容量，用嚟感受規模，唔係精確工程數據。）

---

## 幾點要講清楚

* 觀看人數係**平台自己報**嘅數，各平台點計唔一樣（YouTube 嘅 concurrent viewers
  同 TikTok 嘅 user count 唔係同一個定義），跨平台比較只可以當**粗略量級**睇。
* 榜只收每個平台頭 `TOP_N` 個，所以「全球總和」係**呢批頭部直播主嘅總和**，
  唔係全世界所有直播嘅真總數（真總數冇任何公開 API 攞到）。
* 示範數據入面嘅頻道名全部**虛構**，唔對應任何真人。
* 3D 要 **WebGL2**。冇 WebGL2（好舊嘅機／某啲 embed 環境）會自動跌返落 2D 人海，
  功能一樣，只係唔立體。
