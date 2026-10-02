# Live Arena

直播主照平時咁喺 YouTube／Twitch／Kick 開台，另外喺自己部機開一個 UE5 程式：畫面係一個演唱會場館。

- **台後大 LED** 播你嘅直播畫面，前面係舞台，下面係一圈一圈嘅觀眾席。
- **觀眾撳你分享嘅 link 入場**（手機網頁，唔使裝任何嘢），打個名就攞到門票。**按入場先後派位**：第一個入場坐第一行正中（A 行中間），越早越前。前面嘅觀眾公仔頭上有名牌。
- **YouTube 即時觀看人數**：喺平台睇緊、但冇入場嘅人，會變成無名觀眾坐滿後面，令你見到「好多人睇緊我」。
- **互動模式**：場燈著、搖頭燈掃觀眾席。
- **抽獎**：追光燈喺入咗場嘅觀眾之間跳，越跳越慢，最後停喺一個人身上。
- **請上台**：中獎者個公仔企起身，沿走廊行上台坐嘉賓椅，追光燈一路跟住；佢手機網頁即刻收到你嘅通話 link（Discord／Google Meet 都得）。
- UE 只喺直播主部機行，OBS 擷取 UE 個視窗做直播畫面。觀眾唔使裝 UE。

> **重要：** 呢份代碼喺一部冇 UE 嘅機寫，**未經編譯**。第一次 build 好可能有錯，詳情睇〈[誠實講：限制同風險](#誠實講限制同風險)〉。

---

## 三部分點連

```
 直播主部機（Windows）：UE 只喺呢部機行
 +----------------------------------------------------------------------------
 |
 |  鏡頭 / 遊戲 / 咪
 |      |
 |      v
 |  [OBS 場景「平時直播」]
 |      |
 |      | 虛擬攝影機
 |      v
 |  [Live Arena（UE5）] <-------- WebSocket ---------> [arena server]
 |   LED 播你嘅直播                                  server.py，port 8787
 |   觀眾公仔、燈光、抽獎、上台                      每 30 秒問 YouTube 人數
 |      |                                               ^
 |      | OBS 遊戲擷取 UE 視窗                          |
 |      v                                               |
 |  [OBS 場景「Arena」]                                 |
 |      |                                               |
 +------|-----------------------------------------------|---------------------
        | 推流                                          | 網頁 + WebSocket
        v                                               | 同一個 Wi-Fi：直接連
  [YouTube / Twitch / Kick]                             | 外面嘅人：經 cloudflared（https）
        |                                               |
        v                                               v
  平台觀眾：睇到成個場館                    [觀眾手機網頁]  https://<網址>/r/<房間名>
                                              入場攞票 -> 按先後派位 -> 睇直播、拍手
                                              -> 睇抽獎 -> 中咗收通話 link
```

三個部分：

| 部分 | 喺邊度行 | 做乜 |
|---|---|---|
| **Live Arena（UE5）** | 直播主部機 | 畫場館、LED、觀眾、燈光鏡頭、抽獎。用 WebSocket 連 server 做 host。 |
| **arena server**（`server/server.py`） | 通常都係直播主部機（亦可以放雲端） | 派觀眾網頁、按入場先後派座位、轉發拍手／抽獎／上台、每 30 秒問 YouTube 攞人數。純 Python 標準庫，唔使 `pip install`。 |
| **觀眾網頁**（`server/audience.html`） | 觀眾手機／電腦瀏覽器 | 打名入場、睇門票同座位圖、睇直播、撳拍手、睇抽獎、中咗收通話 link。 |

詳細訊息格式喺 [`PROTOCOL.md`](PROTOCOL.md)。

---

## 你需要

| 項目 | 版本／備註 |
|---|---|
| Windows | 10 或 11，64-bit |
| Unreal Engine | **5.5**（5.4–5.6 都應該得），喺 Epic Games Launcher 裝 |
| Visual Studio 2022 | 裝嗰陣剔 workload **「使用 C++ 的遊戲開發」**（右邊選項確認有剔「Unreal Engine 安裝程式」同一個 Windows 10／11 SDK）。build 嗰陣話缺 SDK／工具鏈，就喺 Visual Studio Installer 再加「使用 C++ 的桌面開發」。 |
| Python | **3.9 或以上**（python.org 下載，安裝時剔「Add python.exe to PATH」） |
| OBS Studio | **30 或以上**（要用到虛擬攝影機嘅「場景」輸出） |
| cloudflared | 選用：想畀唔同 Wi-Fi 嘅觀眾入場先要。`winget install --id Cloudflare.cloudflared` |
| 電腦 | 場館用 Lumen 全局光、虛擬陰影、體積霧，同一部機仲要跑 OBS 編碼，所以要一張好啲嘅獨立顯示卡。**未做過效能測試。** |

---

## 第一次開（示範模式，唔使 server）

### 1. build

1. 喺檔案總管入 `live_arena/LiveArena/`。
2. 右擊 `LiveArena.uproject` → **Generate Visual Studio project files**（Windows 11 要先撳「顯示更多選項」）。
   - 唔見呢個選項都唔緊要，直接 double-click `LiveArena.uproject`，佢問「The following modules are missing or built with a different engine version… Would you like to rebuild them now?」就揀 **Yes**。
   - 你裝嘅唔係 5.5：右擊 `.uproject` → **Switch Unreal Engine version…** 揀你嗰個版本。
3. 第一次會 compile C++ 同 shader，等 10–30 分鐘好正常。
4. 如果彈「LiveArena could not be compiled. Try rebuilding from source manually.」：
   - 開 `LiveArena.sln`，上面揀 **Development Editor**、**Win64**，喺右邊 Solution Explorer 右擊 **LiveArena** → **Build**。
   - 將 **Output** 視窗（或者 Error List）入面嘅錯誤**成段貼返嚟**，我幫你改。UBT 嘅 log 喺 `%LOCALAPPDATA%\UnrealBuildTool\Log.txt`。

### 2. 撳 Play

1. Editor 開咗之後見到一個**黑色空 level**（`/Engine/Maps/Entry`），正常㗎：場館係撳 Play 先由程式砌出嚟。
2. 撳上面綠色 **Play**（或者 `Alt+P`），再**撳一下畫面**等佢收到鍵盤。
3. 你會見到**示範模式**：
   - 假觀眾陸陸續續入場（大約 140 個，由 A 行正中向兩邊、向後坐），頭上有名牌；
   - 隨機有人拍手（公仔跳起、舉螢光棒）；
   - 假嘅平台觀看人數慢慢升到一千幾至幾千，後面坐滿深色無名觀眾；
   - LED 下面條 banner 寫住「示範模式」。
4. 試下撳 `Space`（互動模式）、`L`（抽獎）、`Enter`（請上台）、`K`（落台）、`1`–`4`（鏡頭）。左下角有按鍵表，`H` 收埋。
5. `Esc` 停止 Play；Play 緊想攞返滑鼠就撳 `Shift+F1`。

示範模式只會喺 **冇設定 ServerUrl** 嘅時候行。LED 未有畫面（等待畫面）都正常，睇〈[LED 播你嘅直播](#led-播你嘅直播obs-虛擬攝影機)〉。

---

## 按鍵

同 `ArenaPlayerController.h` 一致。HUD 左下角嘅說明同呢個表一樣。

| 鍵 | 作用 |
|---|---|
| `Space` | 切換 **觀看 ↔ 互動** 模式。互動：場燈著、8 支搖頭燈慢慢掃觀眾席。抽獎途中轉返觀看模式會結束抽獎／嘉賓環節。 |
| `L` | **開始抽獎**（未係互動模式會自動轉）。追光燈喺入咗場嘅觀眾之間跳大約 5 秒，然後停喺一個人度。**抽中之後再撳 `L` = 重抽**（啱啱中嗰位今次唔會再中，除非場內得佢一個）。 |
| `Enter` | **請中獎者上台**：佢網頁即刻收到通話 link；公仔沿走廊、樓梯行上台（視乎坐幾後，大約 10–50 秒），兩支追光跟住，最後坐嘉賓椅。 |
| `K` | **取消抽獎／嘉賓落台**：嘉賓公仔即刻返原位，全場拍手送佢。 |
| `1` | 鏡頭：直播（預設；由觀眾席中間望落舞台同 LED，前排有名牌嘅觀眾喺畫面下半） |
| `2` | 鏡頭：直播主視角（由台上望出去，睇到成個觀眾席） |
| `3` | 鏡頭：全景 |
| `4` | 鏡頭：跟隨（跟住中獎者／嘉賓；冇人中獎時同直播鏡頭一樣） |
| `F` | 自由飛行：`W` `A` `S` `D` 移動、`Q`／`E` 落／升、`Shift` 加速、滑鼠轉方向；再撳 `F` 返原本鏡頭 |
| `[` `]` | 曝光 減／加（每下 0.25 EV，範圍 ±4）。冇方括號嘅鍵盤可以用 `-` `=` |
| `H` | 顯示／收埋按鍵說明 |
| `Tab` | 收埋成個 HUD（OBS 擷取時用，畫面乾淨） |
| `D` | 示範：加 20 個假觀眾。**只係冇設定 ServerUrl 先用得**；自由飛行時 `D` 係向右行 |
| `C` | 示範：30 下拍手（隨機公仔，純畫面效果，有冇 server 都用得） |

數字鍵盤嘅 1–4 都得。鏡頭會自動轉：抽獎開始 → 全景、抽中／上台 → 跟隨、完咗 → 直播。

**抽獎流程細節**

1. 只會喺「撳 link 入咗場」嘅觀眾之中抽，未入場嘅無名觀眾唔會中。
2. 停咗之後 LED 顯示「抽中 名字 · 座位」，中獎者手機見到「你被抽中！」，其他人睇到係邊個。
3. 停咗 **60 秒都冇撳 `Enter`**，會自動取消。
4. 中獎者（或者台上嘉賓）斷線超過 120 秒，會自動取消／落台。
5. 想嘉賓把聲入直播：喺 OBS 加「應用程式音訊擷取」揀 Discord（或者用桌面音訊）。UE 本身冇聲。

---

## 接真觀眾

### 1. 開 server

喺 `live_arena` 資料夾開 PowerShell：

```powershell
py server\server.py
```

（`py` 唔得就試 `python server\server.py`；Mac／Linux 用 `python3 server/server.py`。）

佢會印：

```
  LIVE ARENA server 已經開咗  (Ctrl+C 停)
  觀眾入場 link   http://192.168.1.23:8787/r/<房間名>
  UE host 連線    ws://192.168.1.23:8787/ws
  ...
```

Windows 防火牆問你准唔准 Python 用網絡，揀**私人網路**。

可選參數：`--port 8788`（port 俾人用緊）、`--grace 120`（觀眾斷線保留座位幾多秒）、`--max-viewers 5000`、`--host 127.0.0.1`（淨係經 tunnel 入，唔開放畀同一個 Wi-Fi）、`--verbose`。

### 2. 同一個 Wi-Fi 先試

手機連同一個 Wi-Fi，開 `http://<server 印出嚟嘅 IP>:8787/r/myroom`。見到「LIVE · ARENA」門票就得。

### 3. 畀外面嘅觀眾入場：cloudflared tunnel

另外開一個 PowerShell：

```powershell
cloudflared tunnel --url http://localhost:8787
```

佢會印一個 `https://xxxx-xxxx.trycloudflare.com` 網址。咁：

- 觀眾入場 link：`https://xxxx-xxxx.trycloudflare.com/r/myroom`
- UE 經 tunnel 連：`wss://xxxx-xxxx.trycloudflare.com/ws`

注意：quick tunnel **每次重開網址都唔同**，而且係試用性質（Cloudflare 對同時連線數有限制）。長期用就開 Cloudflare 帳戶，用自己域名開 named tunnel。

### 4. UE 設定

UE editor：**Edit > Project Settings > Game > Live Arena**。改完會自動寫入 `LiveArena/Config/DefaultGame.ini`。

| 欄位 | 例子 | 講乜 |
|---|---|---|
| **ServerUrl** | `ws://localhost:8787/ws` 或者 `wss://xxxx.trycloudflare.com/ws` | server 嘅 WebSocket 網址，**尾一定要有 `/ws`**。server 同 UE 喺同一部機，用 `ws://localhost:8787/ws` 最穩陣。留空 = 離線（示範）。 |
| **RoomId** | `myroom` | 房間名，只可以用英文字母、數字、`_`、`-`，最多 32 字。觀眾 link 尾就係佢。 |
| **HostKey** | 一條長嘅亂碼 | 證明你係房主。**預設 `change-me` 一定要改**。1–128 字。 |
| **JoinUrl** | `https://xxxx.trycloudflare.com/r/myroom` | 顯示喺 HUD 同 LED banner 嘅入場 link。留空就由 ServerUrl 推（`wss://…/ws` → `https://…/r/<RoomId>`）。ServerUrl 係 `localhost` 嘅話**一定要自己填**，因為 localhost link 外面開唔到，LED 唔會顯示佢。 |
| **YouTubeVideoId** | `abcdEFGhijk`（11 個字） | 見〈[YouTube 人數](#youtube-人數)〉 |
| **ScreenSource** | `Auto` | LED 來源：`Auto`／`Capture Device`／`Url`／`None`。見〈[LED 播你嘅直播](#led-播你嘅直播obs-虛擬攝影機)〉 |
| **CaptureDeviceName** | `OBS Virtual Camera` | LED 用名包含呢串字嘅擷取裝置 |
| **StreamUrl** | `https://www.youtube.com/watch?v=…` | 你直播嘅網址：**觀眾網頁會播佢**（YouTube 嵌入、Twitch、Kick 都識）。LED 只係喺 `ScreenSource = Url` 先播佢。冇填 YouTubeVideoId 嘅話，server 會試由呢度拆 YouTube 片 id 攞人數。 |
| **GuestCallUrl** | `https://discord.gg/…` 或者 Meet link | **只會**發畀被請上台嗰位 |
| **StreamerName** | `阿明` | LED banner 同觀眾網頁顯示嘅名 |
| **bDemoMode** | 剔走 | 示範假觀眾。有填 ServerUrl 就一定唔會加假觀眾，不過剔走 HUD 會清爽啲 |
| **MaxFigures** | `2500` | 畫面最多畫幾多個公仔（100–10000）。部機唔夠力就調低 |
| **ExposureBias** | `0` | 開場嘅曝光補償；Play 緊用 `[` `]` 試啱咗再填返嚟 |

改完撳 Play。HUD 左上角會見到「伺服器：已連線 · 房間 myroom」，同埋彈「已連線伺服器」。

> **HostKey 點運作：** 每個房，第一個連入嚟嘅 UE 用佢條 key「認領」個房，server 記住直到 server 重開。之後 key 唔啱就會彈「伺服器拒絕：HostKey 唔啱」，UE 唔會再重連：改 RoomId 或者重開 server。
> 同一個房同一時間只可以有一個 UE：第二部用同一條 key 連入嚟會頂走第一部，第一部彈「另一部機接手咗做 host」並停止連線。

### 5. 試一次完整流程

1. 手機開入場 link → 打名 → **入場**：見到門票、座位號（預設場館第一個人係 **A22**，即 A 行正中）同座位圖 → **進入場館**。
2. UE 入面 A 行正中出現一個有你名牌嘅公仔。撳手機嘅**拍手**，公仔會跳起。
3. 想多幾個觀眾：用電腦瀏覽器嘅無痕視窗再開幾個（每個瀏覽器記住一個座位）。
4. UE 撳 `Space` → `L` → 等停 → `Enter`：手機彈「直播主請你上台」同「上台：加入通話」掣。撳 `K` 落台。

觀眾斷線（例如熄咗手機螢幕）**120 秒內**用同一個瀏覽器返嚟，會坐返原位。座位坐滿（預設場館 2,313 個位）之後再入場會收到「滿座」。

---

## YouTube 人數

冇入場、但喺 YouTube 睇緊嘅人，會變成無名觀眾坐喺最後一個入場觀眾後面（無名觀眾數 = YouTube 同時觀看人數 − 已入場人數）。

1. **攞 API key：** Google Cloud Console → 開一個 project → 啟用 **YouTube Data API v3** → 憑證 → 建立 **API 金鑰**。
2. **開 server 之前設環境變數：**

   ```powershell
   $env:YOUTUBE_API_KEY = "AIza你嘅key"
   py server\server.py
   ```

   （cmd 用 `set YOUTUBE_API_KEY=AIza你嘅key`；想永久記住用 `setx YOUTUBE_API_KEY "AIza你嘅key"`，然後開過一個新視窗。）server 開頭會寫「YouTube 人數 開」。
3. **UE 填 YouTubeVideoId：** 直播網址 `https://www.youtube.com/watch?v=abcdEFGhijk` 入面 `v=` 後面嗰 11 個字。或者 StreamUrl 填直播網址，server 會自己拆（`youtu.be/…`、`/live/…` 都得；頻道頁 `youtube.com/@你/live` 就拆唔到）。
4. 每 30 秒更新一次，每次用 1 個 quota unit（預設每日 10,000）。

注意：

- **每場直播嘅 video id 都唔同**，每次開台都要改（打包版可以用 `-ArenaVideo=` 唔使改設定）。
- 未開播或者已經完咗，YouTube 唔會畀人數，HUD 顯示「平台觀看：未知」，咁就只有入咗場嘅觀眾。原因會寫喺 server log。
- **Twitch／Kick 冇人數**：嗰陣場館淨係有入咗場嘅觀眾。

---

## LED 播你嘅直播（OBS 虛擬攝影機）

LED 最好直接播你 OBS 嘅畫面（冇延遲）：

1. OBS 整兩個場景：
   - **「平時直播」**：你平時嘅鏡頭、遊戲、字幕等。**唔好放 UE 擷取。**
   - **「Arena」**：只係擷取 UE 視窗（見下面〈播出〉）。
2. 撳 OBS 右下「**啟動虛擬攝影機**」（英文介面 Start Virtual Camera）旁邊嘅**齒輪**：
   **輸出類型揀「場景」**，再揀「**平時直播**」。
   ⚠️ 一定要揀一個**唔包含 UE 畫面**嘅場景，否則 LED 入面又有 LED，無限鏡中鏡。
3. 撳「啟動虛擬攝影機」。
4. **然後先開／Play Live Arena**：UE 只會喺開始嗰陣搵一次裝置，`ScreenSource = Auto` 會自動搵名包含 `OBS Virtual Camera` 嘅裝置。
5. HUD 嘅「LED 來源」應該寫「擷取裝置：OBS Virtual Camera」。

**搵唔到／黑畫面：**

- 打開 **Window > Output Log**，搜 `LogLiveArena`，佢會列晒 UE 見到嘅擷取裝置（「擷取裝置：…」）。
- 黑畫面或者完全冇裝置：**Edit > Plugins** 搜「**WMF**」（WMF Media，UE 喺 Windows 靠佢讀擷取裝置）同「**Electra**」（Electra Player），確認有剔，重開 editor。
- 有啲情況 UE（用 Media Foundation 列裝置）會睇唔到 OBS 嘅虛擬攝影機（DirectShow）。咁就：
  - 擷取卡或者真鏡頭都得：CaptureDeviceName 改做佢個名嘅一部分；或者
  - **ScreenSource 改做 `Url`**，StreamUrl 填直播網址：LED 就用內置瀏覽器播網頁（會**有 5–20 秒延遲**；網頁會自動靜音，免得聲音回授）。⚠️ 如果你推流嘅就係 Arena 畫面，LED 播返你條直播即係播返個場館自己（延遲版鏡中鏡）。所以 Url 模式只適合 StreamUrl 唔係 Arena 畫面嘅情況。

---

## 播出（OBS 擷取 UE）

1. 用 **Standalone** 或者打包版開 Live Arena（editor 入面 Play 掣旁邊 `⋮` → **Standalone Game**），咁擷取時唔會影到 editor 介面。想 1920×1080：Editor Preferences > Level Editor > Play 改視窗大小；打包版用 `-windowed -ResX=1920 -ResY=1080`。
2. OBS 場景「Arena」加來源 **遊戲擷取**（模式：擷取特定視窗 → 揀 Live Arena），唔得就用 **視窗擷取**。
3. 喺 UE 撳 **`Tab`** 收埋 HUD（LED 上面嘅字同 banner 係場館一部分，會照出）。
4. 咪、音樂照平時咁加喺「Arena」場景。UE 冇聲。
5. 開始推流。

---

## 打包成 exe

1. Editor 上面 **Platforms > Windows > Package Project**，揀一個資料夾。
2. 完成之後喺 `<資料夾>\Windows\LiveArena.exe`。預設係 Development build，log 會寫喺 `Windows\LiveArena\Saved\Logs\LiveArena.log`，有問題貼返嚟。
3. Project Settings 嘅值會打包入去；以下命令列參數可以蓋過佢哋（唔使重新打包）：

| 參數 | 對應設定 | 例子 |
|---|---|---|
| `-ArenaServer=` | ServerUrl | `-ArenaServer=ws://localhost:8787/ws` |
| `-ArenaRoom=` | RoomId | `-ArenaRoom=myroom` |
| `-ArenaKey=` | HostKey | `-ArenaKey=my-long-secret-key` |
| `-ArenaJoin=` | JoinUrl | `-ArenaJoin=https://xxxx.trycloudflare.com/r/myroom` |
| `-ArenaVideo=` | YouTubeVideoId | `-ArenaVideo=abcdEFGhijk` |
| `-ArenaSource=` | ScreenSource | `auto`／`capture`／`url`／`none` |
| `-ArenaDevice=` | CaptureDeviceName | `-ArenaDevice="OBS Virtual Camera"` |
| `-ArenaUrl=` | StreamUrl | `-ArenaUrl=https://www.youtube.com/watch?v=abcdEFGhijk` |
| `-ArenaCall=` | GuestCallUrl | `-ArenaCall=https://discord.gg/xxxx` |
| `-ArenaName=` | StreamerName | `-ArenaName="阿明 Live"` |
| `-ArenaDemo=` | bDemoMode | `-ArenaDemo=0` |
| `-ArenaMaxFigures=` | MaxFigures | `-ArenaMaxFigures=1500`（100–10000） |

有空格嘅值要用引號。ExposureBias 冇命令列參數，入到去用 `[` `]` 調。

例：喺 `LiveArena.exe` 隔籬整個 `start.bat`：

```bat
@echo off
start "" "%~dp0LiveArena.exe" -windowed -ResX=1920 -ResY=1080 -ArenaServer=ws://localhost:8787/ws -ArenaRoom=myroom -ArenaKey=my-long-secret-key -ArenaJoin=https://xxxx.trycloudflare.com/r/myroom -ArenaVideo=abcdEFGhijk -ArenaDemo=0
```

`my-long-secret-key` 記得換成你自己條 key。`.bat` 檔入面嘅中文好容易變亂碼，所以中文名（StreamerName）最好喺打包之前喺 Project Settings 填好，唔好放喺 `.bat`。

---

## 換上 AI 生成嘅 3D 物件

場館預設用引擎內置嘅方塊、圓柱砌（零 asset）。椅、嘉賓椅、追光燈、觀眾公仔都可以換成你自己（或者 AI 生成）嘅模型。

### 1. 整一個有 ArenaVenue 嘅 level

1. Editor：**Tools > Execute Python Script…** → 揀 `LiveArena/Content/Python/create_arena_level.py`。
   佢會整 `/Game/Maps/Arena`，入面放一個 `ArenaVenue`，存檔，並喺 Output Log 印下一步。
   - Output Log 話搵唔到 `EditorAssetLibrary`：**Edit > Plugins** 剔「**Editor Scripting Utilities**」，重開再行。
   - 手動做法都得：File > New Level > Empty Level → Place Actors 搜 `ArenaVenue` → 拖入去，位置 (0,0,0)、旋轉 0 → 存做 `/Game/Maps/Arena`。
2. **Edit > Project Settings > Maps & Modes**：**Editor Startup Map** 同 **Game Default Map** 都改做 `/Game/Maps/Arena`（Default GameMode 保持 `ArenaGameMode`）。**唔改呢度，撳 Play 用返空 level，你換嘅模型唔會出。**
3. 喺 editor 入面個 level 睇落好黑：正常，燈、霧、觀眾、LED 全部係 Play 先生出嚟。

### 2. 匯入模型（FBX）

Content Browser → **Import**（或者直接拖 FBX 入去），放喺例如 `/Game/Arena/`。

UE 嘅單位係**厘米**、**+X 係前面**、**Z 係上面**。匯入前後要搞掂三樣嘢：

- **Pivot（原點）**：UE 用 FBX 嘅原點做 pivot。喺 Blender 等軟件將物件原點放喺下面寫嘅位置，再 Apply 所有 transform 先 export。
- **方向**：匯入之後拖一個入 level，睇佢正面係咪向紅色箭咀（+X）。唔啱就喺 Blender 轉 90°／180° 再 export，或者匯入時用 Rotation 選項（例如 Yaw 90）。剔 **Force Front X Axis** 會將 FBX 嘅前方轉做 +X。（唔同 UE 版本匯入視窗嘅選項名少少唔同，搵有 Rotation／Uniform Scale／Force Front 字眼嗰幾格。）
- **比例**：雙擊打開 mesh，左上角有 **Approx Size**（厘米）。AI 模型好多時係「1 單位高」，即係 1 cm 咁細：匯入時 **Uniform Scale** 填 100 再 reimport（或者剔 Convert Scene Unit）。
- AI 模型面數通常好多，觀眾席有成千個位，記得剔 **Build Nanite**，或者先減面。

### 3. 放落場館

喺 Outliner 揀 **ArenaVenue** → Details → **Meshes**：

| 欄位 | Pivot 放喺邊 | 正面 | 大約尺寸 |
|---|---|---|---|
| **SeatMesh**（一張觀眾椅） | 地面（椅腳底中間） | +X = 坐低望住舞台嘅方向 | 闊 ≤ 60 cm（座位相隔 62 cm）、深 ≤ 90 cm（行距 95 cm）、座墊大約 46 cm 高 |
| **SeatMeshOffset** | — | — | 椅子位置／角度／大小唔啱，喺度微調（每張椅都套用） |
| **GuestChairMesh**（台上嘉賓椅） | 地面 | +X = 面向觀眾 | 座位大約 45 cm 高（嘉賓公仔坐喺呢個高度） |
| **FollowSpotMesh**（兩支追光燈燈身） | 燈身中間 | +X = 燈口方向 | 大約 40 × 40 × 110 cm |
| **BlockoutMaterial** | — | — | 換方塊嘅材質；一定要有 vector parameter `Color` |

換咗嘅 mesh 用返佢自己嘅材質，唔會被染色。同一個 Details 嘅 **Layout** 可以改行數、半徑、舞台同 LED 大細（改咗座位數會變，UE 連 server 時會自動通知）。改完 **Ctrl+S**，撳 Play。

### 4. 觀眾公仔（ArenaAudience 嘅 FigureMesh）

`AArenaAudience` 有 **FigureMesh** 欄位（pivot = 座位地面點，即同椅子同一點；正面 +X 望舞台；坐低姿勢，屁股大約 45 cm 高、頭中心大約 122 cm 高）。

`create_arena_level.py` 會喺 `/Game/Maps/Arena` 擺一個 **ArenaAudience**。喺 Outliner 揀佢，Details > Audience > **FigureMesh** 揀你嘅模型，Ctrl+S，撳 Play。GameMode 會用 level 入面嗰個，唔會再生過一個新嘅（同 ArenaVenue 一樣）。

如果你嘅 level 係自己開嘅，冇 ArenaAudience：Place Actors 搜「ArenaAudience」拖一個入 level（位置唔重要，公仔係按座位擺）。

用咗 FigureMesh 之後：具名同無名觀眾都用同一個模型（冇深淺色之分）；螢光棒照用圓柱；行上台時公仔係「滑」上去，唔識行路。

---

## 排錯

| 現象 | 試下 |
|---|---|
| HUD 一直「伺服器：連線中…」 | server 有冇開？ServerUrl 尾有冇 `/ws`？`ws://` 定 `wss://` 啱唔啱（tunnel 要 `wss://`）？RoomId 只用咗英文字母、數字、`_`、`-`？防火牆？Output Log 搜 `[Net]` 睇原因 |
| 彈「伺服器拒絕：HostKey 唔啱」 | 個房已經俾另一條 key 認領咗：改 RoomId，或者重開 server。（彈咗之後 HUD 仍然寫「連線中…」，其實已經停咗連線，要重開 Live Arena） |
| 彈「另一部機接手咗做 host」 | 有第二部機用同一個房同 key 連咗入嚟。關咗嗰部再重開呢部 |
| 手機開唔到入場 link | 同一個 Wi-Fi？防火牆准咗 Python？外面嘅人要用 cloudflared 嘅 https link |
| HUD 入場 link 下面寫「localhost：外面嘅觀眾開唔到」 | JoinUrl 填 server 印出嚟嘅 LAN 網址或者 tunnel 網址 |
| 平台觀看：未知 | YOUTUBE_API_KEY 喺開 server **之前**設咗未？video id 啱唔啱？直播開咗未？睇 server log |
| LED 寫「等待直播畫面」 | 見〈LED 播你嘅直播〉：先啟動虛擬攝影機，再開 Live Arena |
| 太暗／太光 | `[` `]` 調曝光，啱咗填入 ExposureBias |
| 好卡 | 調低 MaxFigures（`-ArenaMaxFigures=1000`）；Development build 撳 `` ` `` 開 console 打 `scalability 2` |
| 撳 `D` 冇反應 | 有設定 ServerUrl 就唔准加假觀眾（會彈提示），開入場 link 試 |

UE 嘅 log：editor 用 **Window > Output Log**，搜 `LogLiveArena`；打包版喺 `Saved\Logs\LiveArena.log`。

---

## 誠實講：限制同風險

**代碼**

- 全部 C++ 喺一部**冇 UE 嘅機**寫，**未編譯過、未喺 UE 行過**。第一次 build 有錯好正常：將 Output 或者 Output Log 嘅錯誤成段貼返嚟。
- 有幾個 API 簽名係憑記憶寫（例如 `UWebBrowser::ExecuteJavascript`、`bAffectDistanceFieldLighting`、`FString::RightChopInline`），最有可能喺呢啲位出錯。
- 燈光亮度、曝光、直播鏡頭構圖都只係計數、冇喺機度睇過；鏡頭構圖係按預設場館（28 行、110°）計，改咗 Layout 可能要再調。
- `StreamUrl` 主要係畀觀眾網頁播；淨係 `ScreenSource = Url` 先會喺 LED 播（Auto 唔會，避免場館入面見到慢幾秒嘅自己）。
- 冇設 GuestCallUrl 都可以請人上台，但嘉賓網頁冇通話掣（HUD 會提你）。
- LED 用網頁模式時，網頁每秒靜音一次，頭一秒可能有聲。

**平台同法律**

- 觀眾網頁會嵌入你嘅直播（YouTube 用官方嵌入播放器）。要遵守 YouTube 條款同 YouTube API 服務條款，YouTube Studio 要准許嵌入；有啲直播（例如兒童內容）唔可以嵌入，網頁會有「喺直播平台開」link 補救。
- Twitch 播放器要求網頁用 https，同一個 Wi-Fi 用 `http://` link 嘅時候 Twitch 畫面可能出唔到；Kick 嵌入未實測過。
- **抽獎要跟平台規則同當地法例**：YouTube／Twitch／Kick 對 giveaway／contest 都有政策（例如要寫清楚規則同獎品、唔可以要觀眾畀錢先參加）。呢個程式只係喺撳咗入場 link 嘅人入面隨機揀一個，**唔會幫你處理規則、資格、派獎**。同一個人用幾個瀏覽器可以入場幾次。
- 觀眾自己打嘅名會出現喺直播畫面（名牌、LED）。**冇粗口過濾、冇踢人功能**，開放畀陌生人入場之前要諗清楚。

**server**

- 單一 Python process，**全部記喺記憶體**：重開 server 會清晒座位、房間同 HostKey 認領。
- 冇防洗版：有心人可以用 script 不停入場坐滿個場。
- 一個房嘅具名觀眾上限 = 場館座位數（預設 2,313）同 `--max-viewers`（預設 5,000）兩者細嗰個；坐滿之後嘅人入唔到。UE 連線之前已經入咗場而又多過座位數嘅人，UE 唔會畫。

**安全**

- **HostKey 一定要改**，唔好用 `change-me`，亦唔好喺直播畫面打開 Project Settings。
- 房間名喺入場 link 同 LED 公開；如果有人比你早用你個房間名同另一條 key 連 server，你會收到「HostKey 唔啱」：改 RoomId 或者重開 server。
- 同一個 Wi-Fi 嘅連線係 `http`／`ws`（冇加密）。對外請用 cloudflared 嘅 `https`／`wss`；只經 tunnel 入嘅話可以用 `--host 127.0.0.1` 開 server。

---

## 檔案結構

| 路徑 | 內容 |
|---|---|
| `README.md` | 呢份說明 |
| `PROTOCOL.md` | server ↔ UE ↔ 觀眾網頁嘅訊息格式、座位公式 |
| `server/server.py` | arena server：HTTP 派網頁、WebSocket、YouTube 人數（純標準庫） |
| `server/audience.html` | 觀眾入場網頁（門票、座位圖、直播、拍手、抽獎、上台） |
| `server/selftest.py` | server 自動測試：`py server\selftest.py` |
| `LiveArena/LiveArena.uproject` | UE 專案（UE 5.5；開咗 WebBrowserWidget、PythonScriptPlugin） |
| `LiveArena/Config/DefaultEngine.ini` | 預設 map（Entry）、GameMode、Lumen、虛擬陰影、60 FPS 上限 |
| `LiveArena/Config/DefaultGame.ini` | Live Arena 設定預設值、打包設定 |
| `LiveArena/Config/DefaultInput.ini` | 用傳統 input（按鍵喺代碼直接讀） |
| `LiveArena/Content/Python/create_arena_level.py` | 整 `/Game/Maps/Arena` 同放 ArenaVenue |
| `LiveArena/Source/LiveArena.Target.cs`、`LiveArenaEditor.Target.cs` | build target |
| `Source/LiveArena/LiveArena.Build.cs`、`LiveArena.h/.cpp` | module 設定、`LogLiveArena` |
| `Source/LiveArena/ArenaTypes.h` | 共用 enum、`FArenaViewer`、座位公式 `ArenaSlots`（同 server 一模一樣） |
| `Source/LiveArena/LiveArenaSettings.h/.cpp` | Project Settings > Game > Live Arena |
| `Source/LiveArena/ArenaGameMode.h/.cpp` | 撳 Play 時砌成個場館、讀設定同命令列、接 server 事件、示範模式、LED banner |
| `Source/LiveArena/ArenaNetSubsystem.h/.cpp` | WebSocket client（自動重連；HostKey 被拒就停） |
| `Source/LiveArena/ArenaVenue.h/.cpp` | 場館：舞台、LED 框、觀眾席、樓梯、桁架；座位位置同走上台路線；可換 mesh |
| `Source/LiveArena/ArenaAudience.h/.cpp` | 觀眾公仔（具名＋無名）、名牌、拍手、行上台 |
| `Source/LiveArena/ArenaScreen.h/.cpp` | LED：擷取裝置／網頁／等待畫面、banner、LED 散光 |
| `Source/LiveArena/ArenaShowDirector.h/.cpp` | 燈光、霧、曝光、鏡頭、抽獎同上台流程 |
| `Source/LiveArena/ArenaPlayerController.h/.cpp` | 按鍵、自由飛行、HUD 狀態 |
| `Source/LiveArena/ArenaWidgets.h/.cpp` | 全部 UI（LED 內容、banner、名牌、HUD），純 C++ 砌，冇 .uasset |

第一次 build 之後會多咗 `Binaries/`、`Intermediate/`、`Saved/`、`DerivedDataCache/`、`.vs/`、`LiveArena.sln`：全部係自動生成，唔使 commit。

---

## 開發者

- 改 server 之後跑 `py server\selftest.py`（Mac／Linux：`python3 server/selftest.py`），應該全部 PASS。
- 座位公式改動要 `server.py` 嘅 `center_out`／`slot_label` 同 `ArenaTypes.h` 嘅 `ArenaSlots` 一齊改。
