# 1142 學期 計算機程式語言 期末專題：以 Python Flask 作為 Web 前端、C 語言作為核心引擎的 Rogue-lite 彈幕生存遊戲

本專題將 **C 語言**（指標、動態記憶體配置）作為遊戲邏輯的權威核心，以 **Python Flask** 提供 Web 路由與前後端橋接，瀏覽器 Canvas 負責即時渲染。三者透過 `subprocess` 以 stdin/stdout JSON 協定整合，形成清晰的分層架構：**表現層（Browser）→ 應用層（Flask）→ 引擎層（C）**。專題設計理念在於實踐「系統跨語言整合」與「底層記憶體生命週期管理」——敵人與掉落物以鏈結串列動態生成與銷毀，子彈則以固定陣列池避免海量彈幕造成的堆積碎片，展現針對不同實體特性的資料結構取捨能力。

## 專題名稱與功能介紹

**專題名稱：** Rogue-lite Bullet Hell（Web 版彈幕肉鴿生存遊戲）

**實際實作功能：**

- **角色移動**：方向鍵 / WASD 八方向移動；前端以 `(x, y)` 為視覺權威座標，每約 32ms 透過 API 同步至 C 引擎，並以 `clamp_player` 限制在 800×600 邏輯世界內。
- **自動射擊**：玩家無需手動開火；C 引擎每 tick 以 `enemy_find_nearest` 遍歷敵人鏈結串列，瞄準最近敵人發射子彈，射速受 `fire_rate` 與冷卻計時控制。
- **敵人波次生成（Wave 1～4）**：依波次遞增生成配額、同場上限與生成間隔；敵人 HP 與速度隨波次成長，營造包圍與壓力感。
- **第 5 波 Boss 戰**：單一高 HP Boss，交替施展 **360° 環狀彈幕**（18 發）與 **鎖定玩家的扇形追蹤散彈**（15 發）；擊敗後進入勝利狀態。
- **彈幕碰撞判定**：圓形半徑近似碰撞——玩家子彈 ↔ 敵人、Boss 子彈 ↔ 玩家、玩家 ↔ 敵人接觸持續傷害。
- **掉落與升級機制**：敵人死亡隨機掉落傷害提升或射速提升道具；撿取後套用**邊際效益遞減** Buff，避免數值無限膨脹。
- **脫戰回血**：連續 3 秒（180 tick）未受傷後，每秒恢復固定 HP。
- **死亡與重開條件**：玩家 HP 歸零 → `GAME_OVER`；擊敗 Boss → `WIN`；可透過 `restart` 重置並釋放所有動態記憶體。
- **分數系統**：擊殺一般敵人 +10 分，擊敗 Boss +2000 分。
- **Web HUD**：波次、分數、Boss 血條、狀態 Overlay（主選單 / 遊戲中 / 結束 / 勝利）。

## 使用技術

| 技術關鍵字 | 在本專案中的應用 |
|-----------|-----------------|
| **Pointer（指標）** | `Game.enemy_head` / `loot_head` 為鏈結串列頭指標；遍歷時以 `cur = cur->next` 前進；刪除節點前**先快取 `next` 再 `free`**，避免 Dangling Pointer 與 Segmentation Fault。子彈池以 `&game->bullets[i]` 指向固定陣列槽位，生命週期由 `active` 旗標管理。 |
| **Malloc / Free** | 敵人（`Enemy`）與掉落物（`Loot`）以 `malloc(sizeof(...))` 配置，`enemy_list_remove_node` / `loot_list_remove_node` 或 `*_list_clear` 配對 `free`；遊戲重開時 `entity_reset_all` 一次清空整條鏈，杜絕 Memory Leak。 |
| **資料結構（Linked List）** | 敵人與掉落物採**單向鏈結串列**（頭插法 `push`、指定節點 `remove`、全清 `clear`），動態應對不斷出現與消失的實體；符合評分規範「擇一即可」之鏈結串列要求。 |
| **Struct（結構體）** | `Player`、`Enemy`、`Bullet`、`Loot`、`Game` 封裝座標、HP、速度、波次、分數等；`Game` 聚合玩家、鏈結串列頭指標與子彈固定陣列，作為引擎唯一上下文。 |
| **Python Flask & Subprocess** | Flask 提供 `/` 頁面、`GET /api/state`、`POST /api/action`；`subprocess.Popen` 啟動 `game_engine`，背景執行緒讀取 stdout `STATE` 行、寫入 stdin JSON 指令，實現 Browser ↔ C 的跨語言即時通訊。 |
| **Object Pool（加分設計）** | 子彈使用 `Bullet bullets[MAX_BULLETS]` 固定池（512 槽），以 `active` 重用槽位，**不對每顆子彈 malloc/free**，避免 Boss 環彈等高頻場景的記憶體碎片化。 |

## 系統架構與執行方式 (How to run)

### 系統架構

```
Rogue-lite-Bullet-Hell-main/
├── app.py                      # Flask 主程式、subprocess 橋接
├── requirements.txt            # Python 依賴（Flask）
├── README.md
├── backend/
│   ├── main.c                  # 遊戲主迴圈、Wave、AI、碰撞、JSON I/O
│   ├── entity.c                # 敵人/掉落鏈結串列、子彈池
│   ├── entity.h
│   ├── game.h                  # Struct 定義與遊戲常數
│   ├── Makefile
│   └── game_engine(.exe)       # 編譯產物（需 make 產生）
└── frontend/
    ├── templates/
    │   └── index.html          # 遊戲頁面與 Canvas
    └── static/
        ├── game.js             # 輪詢狀態、輸入、渲染、插值
        └── style.css
```

**資料流向：**

```
Browser (game.js)
  -- GET /api/state  -->  Flask (app.py)  <-- stdout: STATE {...}  --  C Engine (main.c)
  -- POST /api/action -->  Flask            -- stdin: JSON line  -->  C Engine
```

1. 瀏覽器以約 25ms 間隔輪詢 `GET /api/state`，取得 C 引擎最新遊戲狀態 JSON。
2. 玩家按鍵 / 開始 / 重開時，`POST /api/action` 將指令轉發至 C 引擎 stdin。
3. C 引擎以固定 60 Hz tick 更新邏輯，每 tick 輸出一行 `STATE {...}` 至 stdout；除錯訊息僅寫 stderr，不污染 JSON。

### 執行環境與步驟

**需求：**

- C 編譯器：`gcc`（或 MinGW-w64 on Windows）
- Python 3.10+
- 可選：`make`（或使用下方 gcc 手動編譯）

**步驟 1：編譯 C 引擎**

```bash
cd backend
make
```

若無 `make`（Windows 可手動編譯）：

```bash
cd backend
gcc -Wall -Wextra -std=c99 -O2 -c main.c entity.c
gcc -Wall -Wextra -std=c99 -O2 -o game_engine main.o entity.o -lm
# Windows 產物為 game_engine.exe，Flask 會自動偵測
```

**步驟 2：安裝 Python 依賴**

```bash
pip install -r requirements.txt
```

**步驟 3：啟動 Flask**

```bash
python app.py
```

**步驟 4：開啟瀏覽器**

訪問 http://127.0.0.1:5000 ，點擊「開始遊戲」，以方向鍵或 WASD 移動。

## 對外 API 規格 (C ↔ Python)

### Flask HTTP API（Browser ↔ Flask）

| 方法 | 路徑 | 說明 |
|------|------|------|
| `GET` | `/` | 回傳遊戲 HTML 頁面 |
| `GET` | `/api/state` | 回傳最新遊戲狀態 JSON（含 C 狀態 + Flask 中繼欄位） |
| `POST` | `/api/action` | 轉發玩家指令至 C 引擎 stdin |

**`POST /api/action` 請求範例：**

```json
{"action":"start"}
```

```json
{"action":"restart"}
```

```json
{"action":"move","dx":1,"dy":0,"x":412.5,"y":300.0}
```

| `action` | 欄位 | 說明 |
|----------|------|------|
| `start` | — | 從 MENU / GAME_OVER / WIN 進入 PLAYING |
| `restart` | — | 重置遊戲（釋放鏈結串列記憶體後重開） |
| `move` | `dx`, `dy`（-1～1）, `x`, `y`（float） | 更新玩家位置與移動方向 |

成功回應：`{"ok":true,"sent":{...}}`

### C 引擎 stdin / stdout 協定（Flask ↔ C）

**stdin（Flask → C）：** 每行一個 JSON 物件，以換行結尾。

```
{"action":"start"}
{"action":"move","dx":0,"dy":-1,"x":400.0,"y":280.5}
```

**stdout（C → Flask）：** 每 tick 一行，格式為 `STATE` + 空格 + JSON + 換行。

```
STATE {"state":"PLAYING","wave":1,"tick":42,"score":30,...}
```

**stderr：** 僅引擎除錯日誌（波次進度、Boss 攻擊、malloc 警告等），不進入狀態管線。

### C 狀態 JSON 欄位定義

| 欄位 | 型別 | 說明 |
|------|------|------|
| `state` | string | `MENU` / `PLAYING` / `GAME_OVER` / `WIN` |
| `wave` | int | 當前波次（1～5） |
| `tick` | number | 邏輯帧計數 |
| `score` | int | 分數 |
| `tickRate` | int | 固定 60 |
| `player` | object | `x`, `y`, `hp`, `maxHp`, `damage`, `fireRate`, `speed`, `dx`, `dy` |
| `enemies` | array | `{id, x, y, hp, maxHp, boss, speed}` |
| `bullets` | array | `{id, x, y, vx, vy, owner}`（`owner`: 0=玩家, 1=Boss） |
| `loot` | array | `{id, x, y, type}`（`type`: 0=傷害, 1=射速） |

Flask 在 `GET /api/state` 時於 JSON 開頭拼接中繼欄位（字串拼接，不重新 parse C JSON）：

- `engine_ok`：C 引擎是否正常
- `engine_error`：錯誤訊息或 `null`
- `serverTime`：伺服器時間戳
- `clientPollMs`：建議輪詢間隔（15）

## 可選擴充 / 加分項目

### 鏈結串列 vs 物件池：彈幕場景的雙軌記憶體策略

彈幕遊戲同時面臨兩類生命週期截然不同的實體：

| 實體類型 | 數量特性 | 本專案策略 | 理由 |
|---------|---------|-----------|------|
| 敵人 / 掉落物 | 數量中等、存活時間長 | 鏈結串列 + malloc/free | 動態實體語意；重開時 `*_list_clear` 保證無洩漏 |
| 子彈 | 數量極大、存活極短 | 固定陣列 Object Pool | 避免 Boss 環彈等高頻 malloc/free 造成堆積碎片 |

**指標安全實踐（鏈結串列刪除）：**

```c
Enemy *dead = e;
e = e->next;
enemy_list_remove_node(g, dead);
```

**物件池實踐（子彈）：**

```c
for (int i = 0; i < MAX_BULLETS; i++) {
    if (!game->bullets[i].active) {
        game->bullets[i].active = 1;
        return 1;
    }
}
```

此設計展現對 **malloc 生命週期** 的深入理解：應依存取頻率與存活時間選擇鏈結串列或池化，是遊戲引擎中常見的進階記憶體管理手法。

### 其他已實作之穩定性設計

- **Boss spawn 防呆**：`malloc` 失敗時不設定 `boss_spawned`，下 tick 重試，避免誤觸勝利判定。
- **引擎健康監控**：Flask 背景執行緒偵測 stdout 逾時或程序崩潰，自動重啟 C 子行程。
- **前端插值**：敵人 Lerp、子彈 Dead Reckoning，在 60Hz 邏輯與高刷新率渲染之間取得流暢視覺。

## Cursor Prompt 使用紀錄

（請自行填寫）

## 授權 (License)

MIT License