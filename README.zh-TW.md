<div align="center">
  <p><a href="README.md">English (en-US)</a> | <strong>繁體中文 (zh-TW)</strong></p>

  <img src="Full-icon.png" alt="MORS 龍形標誌" width="480">

  一款以 C++23 編寫的現代 UCI 西洋棋引擎。

  [![C++23][cpp-badge]][cpp-link]
  [![UCI][uci-badge]][uci-link]
  [![License][license-badge]][license-link]
  [![Last commit][commits-badge]][commits-link]
</div>

**MORS** 是由 [Theodore Magnus Øen & Codex](AUTHORS) 獨立實作、持續開發中的 C++23 西洋棋引擎。MORS 透過[通用西洋棋介面（UCI）][uci-link]通訊，因此既能在終端機中使用，也能安裝到支援 UCI 的西洋棋圖形介面。

目前的正式版本為 **Dragon of MORS 0.2.0**。MORS 支援標準西洋棋與 Chess960，使用持久化 Lazy SMP 搜尋執行緒，可設定 1–22,528 執行緒，預設為 1。

## Rating

| 版本 | MLTC 相對 Elo | MSTC 相對 Elo | MDFRC 相對 Elo | CCRL 40/15 |
|---|---:|---:|---:|---:|
| Dragon of MORS 0.2.0 64-bit | +109.28 ±16.23（440 局） | +136.88 ±7.23（3,538 局） | +152.86 ±17.98（822 局） | |
| MORS 0.1.0 64-bit | 0 | 0 | 0 | [3464 ±51](https://computerchess.org.uk/4040/cgi/engine_details.cgi?match_length=30&print=Details&each_game=0&eng=MORS%200.1.0%2064-bit#MORS_0_1_0_64-bit) |

### 本地測試定義

| 測試 | 時限 | Hash | 通常樣本數（局） |
|---|---|---:|---:|
| **MLTC** | 120+1 | 256 MB | 800–1,500 |
| **MSTC** | 5+0.05 | 8 MB | 3,000–8,000 |
| **MDFRC（Double FRC）** | 10+0.1 | 16 MB | 800–1,500 |

所有測試皆為每個引擎一個執行緒，同一開局交換先後手配對。時限單位為秒：初始時間＋每步增益。

| 測試 | 開局庫 |
|---|---|
| MLTC / MSTC | `UHO_4060_v4.epd` |
| MDFRC | `DFRC_4852_v1.epd` — Double Fischer Random Chess，啟用 Chess960 模式 |

> [!NOTE]
> Hash 為每個引擎配置的置換表記憶體。

## 快速開始

MORS 透過 UCI 接收指令。如需圖形棋盤，請在支援 UCI 的西洋棋 GUI 中新增引擎，選取編譯好的 `.exe`。預設神經網路已內建。

在已備妥[編譯環境](#環境需求)的 MSYS2 UCRT64 shell 中執行：

```bash
git clone https://github.com/TMagnusN/mors.git
cd mors
make -C src -j$(nproc) CONFIG=release ARCH=generic
./build/release-generic/Dragon-of-MORS-generic.exe
```

> [!IMPORTANT]
> 請選擇符合 CPU 指令集的版本。`generic` 適用於基本 x86-64 CPU；`avx2+bmi2` 需要同時支援 AVX2 與 BMI2。詳見[指令集版本](#指令集版本)。

啟動後依序輸入以下指令。收到 `uciok` 後輸入 `isready`，收到 `readyok` 後再設定局面並開始搜尋：

```text
uci
isready
position startpos
go movetime 1000
```

> [!IMPORTANT]
> 等待引擎回傳 `bestmove` 後，再輸入 `quit` 結束。若要提早停止搜尋，輸入 `stop` 並等待 `bestmove`。

## 導覽

[Rating](#rating) · [主要功能](#主要功能) · [使用與設定](#使用-mors) · [Syzygy](#syzygy-殘局庫) · [神經網路](#神經網路評估) · [編譯](#編譯) · [測試](#測試) · [倉庫結構](#倉庫結構)

## 主要功能

- 使用 aspiration window 的 iterative-deepening Principal Variation Search，以及共享 TT 的持久化 Lazy SMP workers。
- 增量更新的 P2-H32 神經網路評估。
- Clustered transposition table，可透過 UCI `Hash` 設定 1–8,589,934,592 MiB（8 PiB）。
- Check-aware quiescence search，整合 TT 讀寫、non-PV bound cutoff、raw static-eval 快取、SEE 與 late-move pruning；未被將軍時只生成 noisy moves。
- Static Exchange Evaluation，以及 TT、killer、countermove、butterfly、continuation 和 noisy history 走法排序；各 worker 的私有 history 跨搜尋保留。
- Reverse／forward futility pruning、帶 verification 的 null-move pruning、late-move pruning／reduction、internal iterative reduction，以及包含 multi-cut 處理的 singular extension。
- 標準西洋棋與 Chess960 的合法走法生成、make/unmake、Zobrist hashing、重複局面、五十步規則與子力不足和棋判定。
- 內建且經 provenance 驗證的預設網路，也可透過 `EvalFile` 載入外部網路。
- 提供 generic x86-64、AVX2、BMI2 與 AVX2+BMI2 四種固定指令集 Windows 建置。

## Syzygy 殘局庫

已整合 MIT 授權的 [Fathom](vendor/fathom/README.md)，原碼以 **C++23** 編譯，
最多支援七子（含雙方國王）。資料檔須另外準備：

```text
setoption name SyzygyPath value D:\syzygy5
setoption name SyzygyProbeLimit value 5
```

`SyzygyProbeLimit` 預設 7，設為 0 停用；實際上限也受已載入資料限制。
`SyzygyProbeDepth` 預設 1，只限制實際最大子數的搜尋節點。
`Syzygy50MoveRule` 預設 true。Windows 多個路徑以分號分隔，
`SyzygyPath` 設為 `<empty>` 可卸載。

> [!IMPORTANT]
> 殘局庫資料檔須另外準備。修改 Syzygy 設定前，請先停止搜尋並等待 `bestmove`。

根節點以 DTZ 評定候選走法，搜尋內部使用 WDL 結果剪枝；仍有王車易位權時
不查庫，Chess960 亦同。缺檔時回到正常搜尋；啟用五十步規則時，只有計數為零
才允許根節點退回純 WDL 查詢。UCI `tbhits` 為主搜尋執行緒的成功查庫次數。
詳見[整合與驗證說明](src/syzygy/README.md)。

## 神經網路評估

MORS 使用可增量更新的 **P2-H32** 神經網路架構：

| 元件 | 形狀 |
|---|---:|
| 特徵映射 | `canonical-hmirror-v1` |
| Coarse branch | 10,240 → 768 |
| Fine branch | 22,528 → 256 |
| Output buckets | 64 |

預設網路為 [`mors-p2h32-s14400M-o3183M-c+frc.mnue`](networks/mors-p2h32-s14400M-o3183M-c+frc.mnue)。它會直接嵌入 release executable，因此執行引擎時不需要在旁邊另外放置網路檔案。

編譯前，Makefile 會用[網路 provenance 紀錄](networks/mors-p2h32.provenance.toml)核對檔案大小與 SHA-256。該紀錄也保留架構、訓練 run、資料集 fingerprint 與 promotion test metadata。目前網路完成 200 個 superbatches，requested training positions 約 144 億。

仍可在執行時載入相容的外部網路：

```text
setoption name EvalFile value C:\path\to\network.mnue
```

將 `EvalFile` 設回預設檔名即可重新使用內建網路。

## 編譯

### 環境需求

MORS 目前使用以 Windows 為主的 GNU Make 建置流程。建議在 **MSYS2 UCRT64 shell** 中使用：

- 支援 C++23 的新版 MinGW-w64 `g++`；
- GNU Make 與 Bash；
- `windres`；
- `sha256sum`、`sed`、`cut`、`tr`、`wc` 等標準 MSYS 工具。

Clone 倉庫並進入目錄：

```bash
git clone https://github.com/TMagnusN/mors.git
cd mors
```

若 CPU 同時支援 AVX2 與 BMI2，可編譯對應的 release：

```bash
make -C src -j$(nproc) CONFIG=release ARCH=avx2+bmi2
```

引擎會輸出到：

```text
build/release-avx2+bmi2/Dragon-of-MORS-avx2+bmi2.exe
```

Release 預設使用 `-O3`、LTO、section garbage collection，並靜態連結 GCC／MinGW C++ runtime。預設 NNUE 與 Windows icon 也會嵌入 executable。

### 指令集版本

| `ARCH` | CPU 需求 | Sliding attacks |
|---|---|---|
| `generic` | Baseline x86-64 | Magic bitboards |
| `avx2` | AVX2 | AVX2-capable path、非 PEXT attacks |
| `bmi2` | BMI2 | PEXT |
| `avx2+bmi2` | AVX2 與 BMI2 | PEXT，並可使用 AVX2 最佳化 |

一次編譯全部 release variants：

```bash
make -C src -j$(nproc) all-versions
```

需要時可用 `LTO=0` 關閉 LTO。`CONFIG=debug` 與 `CONFIG=sanitize` 分別提供 debug 和 sanitizer 建置。

## 測試

針對指定架構執行完整 debug 測試：

```bash
make -C src -j$(nproc) CONFIG=debug ARCH=avx2+bmi2 LTO=0 test-run
```

測試涵蓋 attacks、直接與參考合法走法生成、perft 局面、make/unmake 與 Zobrist 還原、SEE、TT 行為、增量神經網路評估、搜尋 invariants、qsearch TT 存取、worker history 持久化、共享節點限制、時間管理、UCI 行為，以及 BulletFormat datagen records。

也可使用下列獨立 targets：

```bash
make -C src ARCH=avx2+bmi2 perft
make -C src ARCH=avx2+bmi2 ttbench
make -C src ARCH=avx2+bmi2 CONFIG=release datagen
```

Search-distillation generator 的完整說明位於 [`tools/DATAGEN.md`](tools/DATAGEN.md)。

## 使用 MORS

可透過 GUI 設定下列選項，或在終端機中使用 `setoption name <選項> value <值>`。修改設定前先停止搜尋；初次操作請見[快速開始](#快速開始)。

MORS 接受標準六欄 FEN，也接受省略 move counters 的常見四欄或五欄形式。`position fen ... moves ...` 會解析到 `moves` 分隔符為止，以符合一般 UCI GUI 的行為。

使用 Chess960 時，請先啟用 `UCI_Chess960` 再送入局面。MORS 同時接受 X-FEN 的 `KQkq` 權利與 Shredder-FEN 的車檔字母，易位則使用 UCI Chess960 的「王走向參與易位之車」記法。

### UCI 選項

| 選項 | 範圍／預設值 | 說明 |
|---|---|---|
| `Threads` | 1–22,528；預設 1 | 持久化 Lazy SMP workers，共享 TT，各自保有獨立搜尋狀態；實際建立數量受可用系統資源限制。 |
| `Hash` | 1–8,589,934,592 MiB（8 PiB）；預設 256 MiB | Transposition table 容量；實際配置受可用記憶體與位址空間限制。 |
| `NumaPolicy` | `auto`／`none`；預設 `auto` | 可用 CPU 跨 NUMA 節點或 processor group 時綁定 workers，並在使用中的節點建立 NNUE 複本；`none` 交由 OS 排程並共用一份網路。 |
| `Clear Hash` | Button | 清除全部 TT entries。 |
| `SyzygyPath` | 預設 `<empty>` | 殘局庫目錄；Windows 多個路徑以分號分隔，`<empty>` 卸載。 |
| `SyzygyProbeLimit` | 0–7；預設 7 | 查庫子數上限（含雙方國王）；0 停用。 |
| `SyzygyProbeDepth` | 1–100；預設 1 | 實際最大子數局面的最低查庫搜尋深度。 |
| `Syzygy50MoveRule` | 預設 `true` | 查庫時遵守五十步規則。 |
| `UCI_Chess960` | false | 啟用 Chess960 FEN 與易位記法。 |
| `EvalFile` | 預設使用內建網路 | 載入相容的外部 P2-H32 網路。 |
| `Move Overhead` | 0–5000 ms；預設 10 ms | 為 GUI、排程及通訊延遲預留時間。 |

`NumaPolicy=auto` 啟用綁定時會優先使用實體核心，再使用 SMT siblings。
Worker history 在綁定後初始化，NNUE 複本指定偏好的 NUMA 節點；實際頁面位置
由作業系統決定。單節點且單 group 時保留 OS 排程，`none` 停用綁定及複本。
共享 TT 仍使用既有配置器。多個引擎同時執行時，請使用外部 CPU affinity 分配
或 `none`；自動配置不會跨程序保留 CPU。詳見 [NUMA 實作說明](src/platform/README.md)。

## 倉庫結構

| 路徑 | 用途 |
|---|---|
| [`src/chess`](src/chess) | 棋盤表示、attacks、合法走法生成、make/unmake、Zobrist hashing 與 perft。 |
| [`src/search`](src/search) | PVS、quiescence、剪枝、走法排序、時間管理、SEE 與 TT。 |
| [`src/eval/nnue`](src/eval/nnue) | P2-H32 網路載入與增量評估。 |
| [`tests`](tests) | 正確性與 regression tests。 |
| [`tools`](tools) 與 [`src/tools`](src/tools) | Magic generation、perft、TT benchmark 與 search-distillation datagen。 |
| [`networks`](networks) | 內建網路 artifact 與可重現性 metadata。 |
| [`resources/windows`](resources/windows) | Windows executable icon 與 resource script。 |

## 開發狀態

MORS 是持續開發中的實驗性軟體。搜尋技術與神經網路必須先通過正確性檢查及受控引擎對局才會晉升；但短時限測試結果不是通用 rating，不應被當成絕對棋力數字。

目前未內建開局庫走法選擇功能。

## 致謝

MORS 為獨立實作，但開發過程受益於研究開源引擎與工具：

- [Stockfish][stockfish-link]：公開的搜尋工程、UCI、測試及建置思路。
- [Reckless][reckless-link] 與 Weiawaga：乾淨的公開搜尋／剪枝設計比較對象。
- [MagnusChessX][magnuschess-link]：較早期的訓練流程與網路開發歷史。
- [bullet][bullet-link] 與 BulletFormat 生態：神經網路訓練及資料工具。
- [fastchess][fastchess-link] 與 UHO opening suites：可重現的引擎對局測試。

MORS 維持自己的資料結構、網路格式整合、搜尋語義、測試與調參決策。Syzygy 查詢後端採用 [Fathom](vendor/fathom/README.md)，保留其上游原始碼及 [MIT 授權](vendor/fathom/LICENSE)。

## 授權

MORS 以 **GNU Affero General Public License v3.0 or later** 發布。詳見 [LICENSE](LICENSE)。

[repo-link]: https://github.com/TMagnusN/mors
[commits-link]: https://github.com/TMagnusN/mors/commits/main
[license-link]: https://github.com/TMagnusN/mors/blob/main/LICENSE
[cpp-link]: https://en.cppreference.com/w/cpp/23
[uci-link]: https://www.shredderchess.com/chess-features/uci-universal-chess-interface.html
[stockfish-link]: https://github.com/official-stockfish/Stockfish
[reckless-link]: https://github.com/codedeliveryservice/Reckless
[magnuschess-link]: https://github.com/TMagnusN/MagnusChessX
[bullet-link]: https://github.com/jw1912/bullet
[fastchess-link]: https://github.com/Disservin/fastchess
[cpp-badge]: https://img.shields.io/badge/C%2B%2B-23-00599C?style=for-the-badge
[uci-badge]: https://img.shields.io/badge/Protocol-UCI-brightgreen?style=for-the-badge
[license-badge]: https://img.shields.io/badge/License-AGPL--3.0--or--later-success?style=for-the-badge
[commits-badge]: https://img.shields.io/github/last-commit/TMagnusN/mors/main?style=for-the-badge&label=last%20commit
