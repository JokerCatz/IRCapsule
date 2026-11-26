# IR Capsule

[English](README.md) | **繁體中文** | [简体中文](README.zh-CN.md)

用 ESP32-C3 做的紅外線中繼器，也能當藍牙電視遙控器。它學習你現有遙控器的訊號（電視、機上盒、冷氣……），
存起來，需要時再發射。預計裝在透明扭蛋殼裡，透過藍牙（BLE）用靜態網頁設定和操作；可以從網頁觸發，
也可以用一支「家電不會理會」的備用紅外線遙控器觸發。

> **狀態：可運作的原型。** 硬體已經做好，主要功能可用；部分實際使用情境還沒確認。請看[目前狀態](#目前狀態)。

## 運作方式

```
備用遙控器（家電不會理會的代碼） ──紅外線──▶ [VS1838B ×1–4] ─┐
                                                             ▼
                 手機／電腦 ──藍牙（Web Bluetooth）──▶  ESP32-C3 Super Mini ──藍牙遙控器──▶ Android / Google TV
                                                             │ GPIO5
                                                             ▼
                                       [ULN2003] ─▶ [IR333C-A 環形 ×8] ──紅外線──▶ 家電
```

- **錄製：** 接收頭可以接收任何遙控器；每筆訊號以原始的高低電位時間長度保存，並附上協定的推測。
- **重播：** 用硬體產生 38 kHz 載波（ESP32-C3 的 RMT 周邊）發射原始時間序列，藍牙中斷不會干擾載波。
- **觸發：** 網頁上你自訂的按鈕，或備用遙控器的代碼。膠囊只回應清單裡的代碼，並忽略自己發出的光，
  所以不會自己觸發自己。
- **輸出：** 對家電發射紅外線，或透過藍牙對配對的電視送出按鍵。
- 裝置上沒有網頁伺服器、不用 Wi-Fi、不用 SD 卡。所有資料存在晶片的 4 MB 快閃記憶體。

## 目前狀態

| 項目 | 狀態 |
|---|---|
| 錄製：連續擷取、編號草稿、按壓次數、批次命名、草稿試發 | 可用 |
| 訊號：連同名稱、類型、裝置分類、啟用／停用狀態存入快閃記憶體 | 可用，重開機後保留 |
| 紅外線發射（RMT 硬體載波、避免收到自己的訊號） | 可用：作者錄製訊號後重新發射成功。並非每一類家電都試過 |
| 發射環（ULN2003 + 8 顆 LED） | 已完成，用手機相機確認 8 顆都會亮（`ledtest`）。見[接線圖](docs/wiring.html) |
| 修正接收頭造成的冷氣長訊號失真 | 可用 |
| 藍牙電視遙控器 | 作者在一台 TCL Android 電視上確認過按鍵（見下方） |
| 轉送規則：聽到訊號 → 發紅外線、等待、切換圖層、送電視按鍵 | 已在板子上用模擬接收的訊號測過。用實體遙控器透過規則控制真電視**尚未確認** |
| 自訂網頁遙控器、備份／還原 | 可用；在板子和瀏覽器上測過 |
| 存取 header 與加密 | 已測過板子與參考用戶端、以及網頁（透過測試轉接程式）之間的連線。**尚未用手機、Bluefy 或 NFC 標籤試過** |
| 網頁 | 自動測試在無介面瀏覽器執行；**不包含真正的 Web Bluetooth 與 Web Serial 裝置選擇視窗** |

## 藍牙電視遙控器

沒有紅外線代碼的電視功能，膠囊可以和 Android / Google TV 配對成藍牙遙控器（HID over GATT）。作者在一台
TCL Android 電視上確認可用：方向鍵、確定、返回、首頁、輸入來源、電視設定、音量、播放控制、YouTube、Netflix
和電源（關機）。倒轉／快轉在那台電視上沒有反應；其他按鍵尚未測試。不要指望能把電視從待機喚醒。

**關於藍牙識別碼：** 膠囊固定回報 **Google 參考遙控器**的廠商／產品識別碼（`0957:0001`），因為 Android 依這組
識別碼選擇按鍵對照表
（[`Vendor_0957_Product_0001.kl`](https://android.googlesource.com/platform/frameworks/base/+/main/data/keyboards/Vendor_0957_Product_0001.kl)）。
本專案不是 Google 產品、未經 Google 認證，也不應帶著這組識別碼作為商品出貨。它無法通過電視用來配對
原廠遙控器的畫面：請使用一般的藍牙配件清單。

配對方式：網頁 → **裝置** → **配對電視（120 秒）**，然後在 120 秒內到電視上新增膠囊。一定要先按按鈕再配對。
重開機和一般的 `idf.py flash` 之後，配對仍然有效。

## 自訂遙控器與轉送

* **遙控器**分頁：膠囊上最多存 8 支遙控器，每支 3 欄、最多 12 列。每個按鈕可以設定名稱、32 色之一，以及動作：
  電視按鍵（短按或長按）、已儲存的紅外線訊號，或「執行規則」（沒有觸發訊號的規則可以當巨集）。按鈕可設定
  按住連發，間隔 150–1000 毫秒。
* **轉送規則**：接收頭聽到實體遙控器的訊號後依序執行步驟：發射紅外線、等待、切換圖層、**送出電視按鍵**。
  按一次執行一次；按住不會重複執行。

## 存取 header（建議設定）

沒有設定時，藍牙範圍內的任何人都能使用膠囊。可以在網頁（**裝置** → **藍牙存取**）或透過 USB 設定。設定後，
網頁要證明自己知道 header，之後所有傳輸都會加密（AES-GCM）；header 本身不會傳送。網頁會提供給 **NFC 標籤**用的
網址，例如 `https://…/#h=<header>&n=<藍牙名稱>`：開啟它就會自動帶入 header（`#` 後面的部分不會送到網頁伺服器；
瀏覽器可能保留在歷史紀錄）。Web Bluetooth 仍然需要按一下**藍牙**並選擇裝置。USB 永遠不需要 header，也能用來重設。

它防不了的事：拿到 header 或標籤的人可以進入；太短或好猜的 header 可能從錄下的傳輸內容猜出（請使用網頁提供的
22 個隨機字元）；日後 header 外洩，之前錄下的內容也能解開；在還沒設定 header 的藍牙連線上設定它，那一次是
未加密傳送；它也無法阻止掃描、占用連線、配對或干擾。若不想太顯眼，可以替膠囊取自己的藍牙名稱；本專案的
服務代碼不會廣播，但電視遙控器服務必須廣播。

## 備份與還原

在**裝置**分頁。**匯出備份**會把訊號（含草稿）、波形、規則、裝置分類、自訂遙控器和設定（名稱、忽略重複的時間、
圖層）存成一個 JSON 檔。**從備份還原會覆寫**膠囊上的內容：先檢查檔案、下載目前資料、請你確認，寫入後再讀回
比對。如果中途斷線，重新匯入同一個檔案即可。電視配對、存取 header 和草稿的錄到次數不在備份裡。更改的名稱
要重開機後才生效。

## 設定網頁

由 GitHub Pages 提供：<https://jokercatz.github.io/IRCapsule/>（[不接裝置的示範](https://jokercatz.github.io/IRCapsule/?demo=1)、
[接線圖](https://jokercatz.github.io/IRCapsule/docs/wiring.html)）。網頁支援英文、繁體中文和簡體中文：預設跟隨瀏覽器語言，
也可以在頁面上方切換。藍牙和 USB 需要 Chrome 或 Edge。Linux 版預設沒有開啟 Web Bluetooth：請啟用
`chrome://flags/#enable-experimental-web-platform-features` 後重新啟動瀏覽器，或改用 USB。

## 硬體

| 零件 | 數量 | 備註 |
|---|---|---|
| ESP32-C3 Super Mini | 1 | 4 MB 快閃記憶體，板載 LED 在 GPIO8 |
| VS1838B 紅外線接收頭 | 1–4 | 用 3V3 供電，絕對不要接 5V。輸出並接到 GPIO4 |
| IR333C-A 940 nm LED | 8（另備 4） | 4 串，每串 2 顆串聯 |
| ULN2003APG（DIP-16） | 1（另備 1） | 不可用 ULN2004：它的輸入需要 6–15 V |
| 22 Ω 電阻 | 4 | 每串一顆 |
| 100 µF 電解電容 | 1 | 接在 5V 上（+ 接 5V），在 LED 脈衝時穩定供電 |
| 0.1 µF 陶瓷電容（標示 104） | 每顆接收頭 1 顆 | 接在每顆 VS1838B 的 VCC 與 GND 之間。ULN2003 旁可再加一顆（選配） |

完整電路圖、腳位表、環形排列和通電前檢查：**[docs/wiring.html](docs/wiring.html)**（用瀏覽器開啟，英文）。使用的腳位：

| GPIO | 功能 |
|---|---|
| 4 | 紅外線接收（VS1838B OUT，閒置時為高電位） |
| 5 | 紅外線發射（ULN2003 IN1–IN4） |
| 8 | 板載狀態燈（不是外接零件） |

**焊接後檢查 LED：** 連上網頁，開啟**裝置**分頁，按**開始 LED 測試**（協定指令 `ledtest`）。所有發射 LED 會閃爍
60 秒，用手機相機觀看。每一串是兩顆串聯，所以一顆壞掉或接反，那一串的兩顆都不會亮。

## 編譯與燒錄

需要 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v6.1。

```sh
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash        # C3 的 USB-Serial/JTAG 序列埠，不需要按 BOOT 鍵
```

如果同時接了好幾塊板子，建議使用固定的 `/dev/serial/by-id/...` 路徑。用 esptool 或 pyserial 開啟某塊板子的
序列埠可能讓它重開機，和其他專案共用一台電腦時要小心。

## 網頁設定介面

`web/index.html` 是單一靜態檔案。因為 Web Bluetooth 需要安全環境，請透過 HTTPS（GitHub Pages 即可）或
`http://localhost` 提供。

```sh
cd web && python3 -m http.server 8000      # 然後開啟 http://localhost:8000
```

Android 或電腦上使用 Chrome 或 Edge。iPhone 請用支援 Web Bluetooth 的瀏覽器，例如 Bluefy。Web Serial（USB）需要
電腦版 Chrome 或 Edge。網址加上 `?demo=1` 可以用模擬的膠囊試用。

一般使用流程：

1. **錄製**（**紅外線訊號**分頁）：按**開始錄製**，然後拿任何遙控器對著接收頭按鍵。每個新的按鍵會成為一筆編號草稿
   並附波形；重複按同一個鍵只會增加次數。**試發**可以立刻發射草稿。
2. **命名**：輸入名稱（或貼上清單，每行一個，依錄製順序套用），選擇類型和裝置分類，按**儲存已命名的草稿**。
   只有命名過的訊號能用在按鈕和規則。
3. **發射**：在**紅外線訊號**分頁，或用**遙控器**分頁上你自訂的按鈕。
4. **轉送規則**：**當聽到**某個已儲存的訊號（例如備用遙控器的一個鍵），**就執行**一串步驟。規則和訊號可以停用而
   不必刪除。
5. **裝置**：電視配對、藍牙存取 header、備份、設定、LED 測試和裝置紀錄。

## 命令列工具

開發用。需要 [pyserial](https://pypi.org/project/pyserial/)；部分工具還需要 `bleak`、`websockets` 或 `cryptography`。
工具的輸出和說明是英文。

```sh
export IRCAP_PORT=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00

python3 firmware/tools/selftest.py          # 錄製、命名、發射、規則、保存；保留你的資料
python3 firmware/tools/relay_check.py       # 「按一次執行一次」與電視按鍵規則步驟
python3 firmware/tools/ui_check.py          # 遙控器版面儲存（需要兩個空位）
python3 firmware/tools/ledtest_check.py     # LED 測試指令
python3 firmware/tools/burst_check.py       # 連續送出多個指令
python3 firmware/tools/access_check.py      # 存取 header 與加密（USB 加上這台電腦的藍牙）
python3 firmware/tools/remote_check.py      # 電視遙控指令。警告：會忘記已配對的電視
python3 firmware/tools/bridge.py 8765       # USB <-> WebSocket，用來測試網頁
python3 firmware/tools/capture.py "$IRCAP_PORT" captures/tv.jsonl 60 tv   # 把原始訊號記錄到檔案
```

這些工具大多會暫時發射紅外線或改動儲存資料；執行前請先讀工具開頭的說明。範圍內已配對的電視會自己連回來，
所以任何會送出電視按鍵的操作，都會真的控制它。`securelink.py` 是加密藍牙協定的參考用戶端。

通訊協定寫在 [docs/protocol.md](docs/protocol.md)（英文）。開啟序列埠會讓板子重開機，所以插上後請等幾秒再執行工具。

## 目錄結構

```
firmware/        ESP-IDF 專案
  main/ir_rmt.c    RMT 擷取與發射
  main/engine.c    訊號緩衝、草稿、觸發比對、巨集執行、紀錄
  main/store.c     快閃記憶體儲存（NVS）
  main/proto.c     指令解譯
  main/ble.c       GATT 傳輸
  main/secure.c    存取 header：在逐行協定之下的交握與加密
  main/remote.c    電視遙控器（BLE HID）
  main/ui_store.c  自訂網頁遙控器的儲存
firmware/tools/  電腦端測試與開發工具（Python）
web/index.html   設定網頁（單一靜態檔案）
docs/wiring.html 電路圖與組裝指南（單一檔案，用瀏覽器開啟，英文）
docs/protocol.md 通訊協定（英文）
docs/spec.zh-TW.md  開工前的完整設計規格與討論（繁體中文）
```

## 已知限制

- VS1838B 會解調訊號，因此無法量到被錄遙控器的載波頻率。重播預設 38 kHz；其他載波（36、40、56 kHz）要手動設定。
- RMT 的時間長度是 15 位元，超過 32 767 µs 的項目在發射時會拆成多段。
- 靜默 30 毫秒後視為一筆訊號結束（1 MHz 下的硬體閒置上限），所以中間間隔較長的冷氣訊號會分成幾筆收到。
  可以用 `gap` 欄位把它們歸在一起。
- **冷氣每次都傳送整個面板狀態**（模式、溫度、風量……）。因此錄下的訊號會把家電設成錄製當下的狀態。想要幾種狀態
  就各錄一筆（例如「冷氣 26 °C」、「關機」）。目前沒有實作單獨調整某個欄位（升溫／降溫）。
- 開機後可用記憶體約 73 KB（含藍牙堆疊）。可儲存 96 個訊號和 48 條規則，每個訊號最多 1200 個項目；限制在
  記憶體，不是快閃記憶體。
- 沒有設定存取 header 時，藍牙範圍內的任何人都能連線、改設定、對已配對的電視送按鍵，並開啟配對時間。藍牙配對
  本身一律接受（Linux 主機需要），但按鍵只送給在配對時間內選定的電視。
- 只錄製和重播你擁有或有權控制的裝置的遙控訊號。

## 授權

[MIT](LICENSE)。
