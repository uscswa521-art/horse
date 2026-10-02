# Live Arena 協議 / Protocol

`server/server.py` 同時做三樣嘢：HTTP 派觀眾網頁、WebSocket 轉發事件、（有 key 嘅話）定時攞 YouTube 即時觀看人數。
UE 程式（host）同觀眾網頁（viewer）都連同一個 WebSocket endpoint。全部訊息都係 UTF-8 JSON text frame，`t` 係類型。

## HTTP

| Path | 內容 |
|---|---|
| `GET /` | 簡單首頁：輸入房間名 → 跳去 `/r/<room>` |
| `GET /r/<room>` | 觀眾入場頁 `server/audience.html`（房間名由 path 讀） |
| `GET /healthz` | `ok` |
| `GET /ws?...` | WebSocket upgrade |

房間名：`[A-Za-z0-9_-]{1,32}`，其他字元一律 400。

## 連線

```
ws://<host>:<port>/ws?role=host&room=<room>&key=<hostKey>
ws://<host>:<port>/ws?role=viewer&room=<room>
```

- 第一個 host 連入嚟就用佢條 key 認領個房；之後 key 唔啱（或者空 / 長過 128 字）嘅 host 收 `{"t":"error","msg":"bad key"}` 然後斷線（close 1008）。
- 同一個房同一時間只有一個 host；新 host（key 啱）入嚟會頂走舊嗰個：舊嗰個收 `{"t":"error","msg":"replaced"}` 然後斷線（close 4000）。
- Host 收到 `bad key` 或者 `replaced` 就唔好自動重連（重試只會再被拒，或者同另一個 host 不停互相頂走）。
- Host 走咗，房間同座位保留；觀眾照樣入場、拍手，事件會喺 host 返嚟時由 `snapshot` 補返。

## 座位（slot）

- 伺服器按入場先後派 **最細嘅空 slot**：slot 0 = 第一行正中。
- 觀眾斷線後座位保留 **120 秒**，用同一個 `id` 重連就坐返原位；過咗先釋放，並通知 host `leave`。
- 座位標籤公式（server 同 UE 一模一樣，見 `ArenaTypes.h` 嘅 `ArenaSlots`）：
  - 每行 `n` 個位，行內由中間向兩邊填：`mid = n // 2`；第 `j` 個 → `pos = mid + j//2`（j 雙數）或 `mid - (j+1)//2`（j 單數）
  - 標籤 = 行名 + (pos+1)；行名 0..25 = A..Z，26 = AA，27 = AB …
- 行數同每行位數由 host 用 `layout` 話畀 server 知；未收到之前用預設 `[34, 35, 36, …]`（28 行，每行多 1 個，即 34..61）。
- 每個房最多 `5000` 個具名觀眾（`--max-viewers`）；host 送咗 `layout` 之後，上限再收窄到 layout 嘅總座位數（場館坐滿就唔再入）。滿咗收 `{"t":"error","msg":"full"}`。

## Host → Server

| 訊息 | 意思 |
|---|---|
| `{"t":"layout","rows":[34,36,…]}` | 每行位數（1–2000 嘅整數，`34.0` 都當 34）。收到之後所有已入場觀眾嘅 `label` 會重新計，同埋重發 `ticket` 畀佢哋（host 唔會再收 snapshot：host 自己用同一條公式計 label） |
| `{"t":"config","streamer":"阿明","videoId":"abc123","streamUrl":"https://…","callUrl":"https://discord.gg/…"}` | 直播主名、YouTube 片 id（用嚟攞人數）、觀眾頁播嘅直播網址、抽中者上台用嘅通話 link |
| `{"t":"mode","mode":"watch"\|"interactive"}` | 切換模式，server 轉發畀所有觀眾 |
| `{"t":"lottery","phase":"spin"}` | 開始抽獎，server 轉發畀所有觀眾 |
| `{"t":"lottery","phase":"winner","id":"<viewerId>"}` | 抽中邊個，server 補埋 `name`、`label` 轉發畀所有觀眾 |
| `{"t":"guest","id":"<viewerId>"}` | 請佢上台；server 轉發畀所有觀眾（冇 `callUrl`），另外**淨係**畀嗰位觀眾一條有 `callUrl` 嘅 |
| `{"t":"guest","id":""}` | 嘉賓落台；亦代表取消進行中嘅抽獎（冇另外嘅 cancel 訊息） |

## Server → Host

| 訊息 | 意思 |
|---|---|
| `{"t":"snapshot","viewers":[{"id","name","slot","label"}…],"youtube":1234}` | 連線後第一條；`youtube` = -1 代表未知 |
| `{"t":"join","viewer":{"id","name","slot","label"}}` | 新觀眾入場（重連返原位唔會再發） |
| `{"t":"leave","id":"…"}` | 座位釋放（斷線 120 秒後） |
| `{"t":"clap","id":"…"}` | 某觀眾拍手（每人最多每 300ms 一次） |
| `{"t":"count","youtube":1234}` | YouTube 即時觀看人數（每 30 秒） |
| `{"t":"error","msg":"…"}` | 錯誤 |

## Viewer → Server

| 訊息 | 意思 |
|---|---|
| `{"t":"hello","id":"<舊 id 或 null>","name":"阿明"}` | 連線後第一條。名去頭尾空白、刪控制字元、最多 20 字，空白就叫「觀眾」 |
| `{"t":"clap"}` | 拍手 |

## Server → Viewer

| 訊息 | 意思 |
|---|---|
| `{"t":"ticket","id","name","slot","label","room","streamer","streamUrl","rows":[34,35,…]}` | 入場票。`id` 要存喺 localStorage；`rows` = 而家嘅 layout（網頁用嚟畫座位圖）。layout 或者 streamer/streamUrl 改咗會重發 |
| `{"t":"state","mode":"watch","hostOnline":true,"named":56,"youtube":1234,"guest":{"id","name","label"} 或 null}` | 連線後同每次有變（最多每秒一次）都會發 |
| `{"t":"mode","mode":"interactive"}` | 模式轉咗 |
| `{"t":"lottery","phase":"spin"}` | 開始抽獎 |
| `{"t":"lottery","phase":"winner","id","name","label"}` | 抽中邊個（自己 id 相同 = 你中咗） |
| `{"t":"guest","id","name","label","callUrl"?}` | 嘉賓上台；`callUrl` 只會喺發畀嘉賓本人嗰條出現 |
| `{"t":"guest","id":""}` | 嘉賓落台，或者直播主取消咗抽獎（網頁收埋抽獎畫面） |
| `{"t":"error","msg":"…"}` | 錯誤 |

## YouTube 人數

環境變數 `YOUTUBE_API_KEY` 有設，而 host 嘅 `config` 有 `videoId`，server 每 30 秒 call 一次
`https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id=<videoId>&key=<key>`
讀 `items[0].liveStreamingDetails.concurrentViewers`（1 quota unit / 次）。失敗就保持舊值並喺 log 寫原因。
