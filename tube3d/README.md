# 🎬 Tube3D — 行得入去嘅影片大堂

而家上 YouTube，係一格格縮圖攤喺個網頁度。呢個係另一種睇法：

> **每條片變成一塊企喺大堂度嘅立體展板。**
> 你操控一個角色行入去，望住邊塊板就睇到邊條片嘅資料，撳 `E` 就開嚟睇。
> 排頭嘅片擺近門口，**愈多人睇塊板愈大**，大堂入面仲有其他訪客行嚟行去。

```
tube3d.html         ← 個空間本身 (HUD / 小地圖 / 播放器)
tube3d.js           ← 3D 渲染器 (原生 WebGL2, 零外部依賴, 唔使 three.js)
tube3d/collect.py   ← 攞片單
data/videos.json    ← GitHub Action 每 30 分鐘更新
```

---

## 點玩

| 操作 | 做咩 |
|---|---|
| `W` `A` `S` `D` | 行路 |
| `Shift` | 跑 |
| **滑鼠**（撳一下鎖定） | 望嚟望去 |
| `E` | 開你望住嗰條片 |
| **撳一下畫面** | 未鎖滑鼠：鎖定滑鼠；手機：開你望住嗰條片 |
| **撳底下嗰塊卡** | 開片（滑鼠未鎖定嗰陣） |
| `V` | 第一身 ↔ 第三身 |
| **滾輪** | 鏡頭拉遠拉近 |
| `Esc` | 放開滑鼠 |

手機：**左邊撳住拖 = 行路，右邊拖 = 望，撳一下（唔好拖）= 開你望住嗰條片**。

（電腦鎖咗滑鼠之後，畫面上啲卡撳唔到 —— 嗰陣用 `E`，或者 `Esc` 放開滑鼠先。）

右上角有小地圖，紅色箭嘴係你，金色嗰塊就係你而家望住嘅片。

---

## ⚠️ 講清楚：呢個唔係「你嘅 YouTube 首頁」

**YouTube 冇任何公開 API 攞得到你個人化嘅首頁推薦** —— 嗰個係登入狀態下嘅
私人數據，官方 Data API 根本冇呢個 endpoint。任何話做到嘅，都係要攞你嘅
帳號密碼／cookie 去扮你登入，我唔會咁做。

所以呢個大堂入面擺嘅係：

1. **公開熱門榜**（`chart=mostPopular`，可以指定地區）— 預設 HK / US / JP
2. **你自己指定嘅頻道最新片**（`YT_CHANNELS`）— 呢個最接近「你嘅首頁」

畫面左上角會實話實說寫住片單係邊度嚟。

### 令個大堂似返你嘅首頁

喺 GitHub repo → **Settings → Secrets and variables → Actions → Variables**
加一個 variable：

| Name | Value |
|---|---|
| `YT_CHANNELS` | 你跟開嘅頻道，`@handle` 或者 `UC…` id，逗號分隔<br>例：`@mkbhd,@veritasium,UCxxxxxxxxxxxxxxxxxxxxxx` |
| `YT_REGIONS` | *(可選)* 熱門榜地區，預設 `HK,US,JP` |

再加 secret `YOUTUBE_API_KEY`（同直播人潮嗰個共用同一個 key）。

---

## 縮圖點解有時淨係得個色塊

YouTube 縮圖係由 `i.ytimg.com` 出，要**貼落 WebGL 材質就一定要 CORS**。
我用 `crossOrigin="anonymous"` 去載：

* 載得到 → 塊板就係真縮圖
* 載唔到（冇 CORS header / 被擋） → 自動用**標題色塊卡**頂上，唔會爆錯

兩種情況塊板都一定有**標題 · 頻道 · 觀看次數**（呢啲字係我自己畫落 texture 度）。

## 播放：點解要彈個窗出嚟

YouTube 影片本身有 DRM，**冇可能貼落 3D 畫面度做材質**（技術上做唔到，
唔係我懶）。所以撳 `E` 之後會用 **YouTube 官方 embed** 彈個播放器出嚟，
睇完撳「返返個大堂」就繼續行。部分片會被版權方禁止 embed，
嗰陣就要撳「喺 YouTube 開 ↗」。

---

## 自己喺電腦度跑

```bash
# 1. 示範數據 (即刻睇到效果, 標題全部虛構)
python3 -m tube3d.collect --demo

# 2. 真數據
export YOUTUBE_API_KEY=xxx
python3 -m tube3d.collect --regions HK,US --limit 24
python3 -m tube3d.collect --channels @mkbhd,@veritasium --per-channel 6

# 3. 開 http server (⚠️ 唔可以 file:// 直接開)
python3 -m http.server 8899
# 開 http://localhost:8899/tube3d.html
```

### URL 參數

| 參數 | 預設 | 講解 |
|---|---|---|
| `data` | `data/videos.json` | 自己餵第二份片單都得 |
| `people` | `320` | 大堂入面有幾多個其他訪客（0 = 淨係得你） |

---

## quota

| 攞咩 | 幾多 units |
|---|---|
| 一個地區嘅熱門榜 | **1** |
| 一個頻道嘅最新片 | 約 **3**（channels + playlistItems + videos） |
| ~~search~~ | ~~100~~（貴，所以冇用） |

一日限額 10,000。預設 3 個地區 × 每 30 分鐘 = 約 **144 units/日**，
就算加 10 個頻道都仲有大量剩。
（注意同 `streamer_arena` 共用同一個 key，嗰邊用緊約 7,300/日。）

---

## 上線 (GitHub Pages)

1. repo → **Settings → Pages → Source: Deploy from a branch**
2. Branch 揀 **`main`**、folder 揀 **`/ (root)`** → Save
3. 一兩分鐘之後就開得：

```
https://<你嘅 github 用戶名>.github.io/<repo 名>/arena.html      ← 演唱會場館
https://<你嘅 github 用戶名>.github.io/<repo 名>/tube3d.html     ← 影片大堂
```

兩個 workflow 都係 checkout **`main`**、跑完之後將 `data/*.json` **commit 返落 `main`**，
所以 Pages 要 serve `main` 先會見到新數據。
（commit message 特登**冇**加 `[skip ci]` —— 加咗會連 Pages 嘅
`pages build and deployment` 都跳埋，新數據就永遠上唔到線。）

---

## 要 WebGL2

呢個空間要 **WebGL2**。冇嘅話會出提示叫你換瀏覽器／開返硬件加速 —— 唔會白畫面。

在軟件渲染（SwiftShader，最差情況）實測 40 塊板 + 320 個訪客大約 **18 fps**，
真 GPU 上面順好多。訪客太多可以用 `?people=0` 熄咗佢。
