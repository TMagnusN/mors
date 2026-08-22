# MORS `src` 架構設計（C++23）

Release 建置會產生 `mors-<arch>.exe` UCI 引擎。目前支援 `uci`、
`isready`、`ucinewgame`、`setoption name Threads`（固定為 1）、`Hash`、`Clear Hash`、
`Move Overhead`、`position startpos|fen ... moves ...`、`go depth <n>`、
`go nodes <n>`、`go movetime <ms>`、`wtime/btime/winc/binc/movestogo`、
`go infinite`、`stop` 與 `quit`。搜尋在背景執行，`stop` 以 cooperative
cancellation 結束目前 iteration 並回傳最後一個完整 depth 的結果。

本目錄預計承載一個完全獨立設計與實作的現代化西洋棋引擎。UCI、引擎協調、搜尋、棋盤核心、評估與平台最佳化各自有明確邊界。

## 設計目標

- C++23，以 GNU Make 驅動 GCC、Clang 與 MSY2 的 64-bit Release 建置。
- 搜尋熱路徑無虛擬派發、無例外、無動態配置。
- `Position::make_move()` / `unmake_move()`、走法產生與 TT 探查保持資料局部性。
- UCI 只是介面卡；核心不可依賴文字協定、標準輸入輸出或命令列。
- 先做正確且可測的單執行緒核心，再加入 Lazy SMP、NUMA、NNUE 與 Syzygy。
- 每個子系統只能依賴比自己更低的層級，禁止 `engine` / `runtime` / `search` 之間形成循環依賴。

## 目錄配置

```text
src/
|-- Makefile
|-- app/
|   `-- main.cpp
|-- engine/
|   |-- engine.hpp
|   |-- engine.cpp
|   |-- config.hpp
|   |-- request.hpp
|   `-- response.hpp
|-- protocol/
|   `-- uci/
|       |-- session.hpp
|       |-- session.cpp
|       |-- parser.hpp
|       |-- parser.cpp
|       |-- formatter.hpp
|       |-- formatter.cpp
|       |-- option_registry.hpp
|       `-- option_registry.cpp
|-- chess/
|   |-- types.hpp
|   |-- move.hpp
|   |-- bitboard.hpp
|   |-- bitboard.cpp
|   |-- attacks.hpp
|   |-- attacks.cpp
|   |-- position.hpp
|   |-- position.cpp
|   |-- state.hpp
|   |-- zobrist.hpp
|   |-- zobrist.cpp
|   |-- movegen.hpp
|   |-- movegen.cpp
|   |-- fen.hpp
|   |-- fen.cpp
|   `-- perft.hpp
|-- search/
|   |-- README.md
|   |-- score.hpp
|   |-- see.hpp
|   |-- see.cpp
|   |-- tt.hpp
|   |-- tt.cpp
|   |-- search.hpp
|   |-- search.cpp
|   |-- limits.hpp
|   |-- result.hpp
|   |-- stack.hpp
|   |-- move_picker.hpp
|   |-- move_picker.cpp
|   |-- history.hpp
|   |-- time_manager.hpp
|   `-- time_manager.cpp
|-- eval/
|   |-- evaluator.hpp
|   |-- evaluator.cpp
|   `-- nnue/
|       |-- accumulator.hpp
|       |-- accumulator.cpp
|       |-- features.hpp
|       |-- network.hpp
|       |-- network.cpp
|       |-- simd.hpp
|       `-- layers/
|-- runtime/
|   |-- thread_pool.hpp
|   |-- thread_pool.cpp
|   |-- cpu_features.hpp
|   |-- cpu_features.cpp
|   |-- numa.hpp
|   |-- numa.cpp
|   |-- aligned_memory.hpp
|   `-- prefetch.hpp
|-- tablebase/
|   `-- syzygy/
|       |-- probe.hpp
|       `-- probe.cpp
|-- support/
|   |-- assertions.hpp
|   |-- logger.hpp
|   |-- logger.cpp
|   |-- source_location.hpp
|   `-- version.hpp.in
`-- tools/
    |-- benchmark.hpp
    |-- benchmark.cpp
    |-- perft_runner.hpp
    `-- perft_runner.cpp
```

`tests/`、`benchmarks/` 與網路權重檔應放在專案根目錄，不放進 `src/`。

## 分層與依賴方向

```text
app -> protocol/uci -> engine -> search -> eval ------> chess
                               |    |       |             ^
                               |    `------ tablebase ----|
                               `---------- runtime -------|

support/platform primitives are available to all lower-level components.
```

實際規則：

1. `chess` 是純棋局領域核心，不知道 UCI、執行緒、NNUE 檔案或引擎選項。
2. `eval` 只讀取棋盤與增量狀態，回傳 `Score`，不控制搜尋。
3. `search` 擁有 alpha-beta、qsearch、走法排序、history、TT 與時間決策。
4. `runtime` 提供執行緒、CPU feature、NUMA 與對齊記憶體；不包含棋力策略。
5. `engine` 是唯一的生命週期協調者，負責 position、workers、TT、network 與 callbacks。
6. `protocol/uci` 將文字命令轉成強型別 `engine::Request`，並將 `engine::Response` 格式化輸出。
7. `app/main.cpp` 只處理啟動、關閉與最上層錯誤，不放棋力邏輯。

## 核心型別

不要讓所有語意都退化成 `int`。第一階段至少建立：

```cpp
namespace mors {

enum Color : std::uint8_t { WHITE, BLACK, COLOR_NB };
enum PieceType : std::uint8_t { NO_PIECE_TYPE, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum Square : std::uint8_t { A1, B1 /* ... */, H8, SQ_NONE };

using Bitboard = std::uint64_t;
using Key = std::uint64_t;
using Value = std::int32_t;
using Depth = std::int32_t;

class Move; // 16-bit packed value；提供明確的建構與查詢 API

} // namespace mors
```

`Move` 是 trivial、可複製的小型值型別；`Value` 與 `Depth` 使用固定寬度整數，並由 `search/score.hpp` 限定 ordinary、tablebase、mate 與 sentinel 區間。熱路徑的容器採固定容量；搜尋期間不使用 `std::vector` 擴容。

## C++23 使用原則

- 用 `std::expected` 表達 FEN、UCI 命令、NNUE 檔案載入等可恢復錯誤。
- 用 `std::span` 傳遞連續資料，用 `std::bit_*`、`std::byteswap` 與 `<bit>` 處理位元資料。
- 用 `enum class`、concepts 與 `constexpr` 強化走法產生和型別約束。
- 外層工作執行緒可用 `std::jthread` 管生命週期；節點停止檢查仍用 cache-friendly `std::atomic_bool`。
- `std::source_location` 用於斷言與診斷；`std::format` 僅限協定或記錄層。
- 熱路徑函式盡量 `noexcept`，解析與啟動邊界才捕捉例外。
- 第一版不使用 C++ Modules。模組、編譯器 intrinsic、unity build 與 PGO 的工具鏈成熟度不一致，先以 `.hpp/.cpp` 保持跨編譯器穩定。

## Makefile 建置設計

唯一正式建置入口是 `src/Makefile`。所有產物統一輸出至：

```text
build/<config>-<arch>/
```

物件檔與自動產生的 dependency file 位於對應 build 目錄，不在原始碼旁產生 `.o` 或 `.d`。

目前支援：

```text
CONFIG=release
CONFIG=debug
CONFIG=sanitize

ARCH=generic
ARCH=avx2
ARCH=bmi2
ARCH=avx2+bmi2

LTO=0
LTO=1
```

預設為：

```text
CONFIG=release
ARCH=generic
LTO=1
```

主要建置命令：

```text
make
make build
make release

make debug
make sanitize
make test

make uci
make perft
make ttbench
make datagen

make all-versions
make clean
```

`make` / `make build` 會建置目前 `CONFIG` 與 `ARCH` 對應的 UCI 引擎與 perft executable。

Release 產物依架構命名：

```text
mors-generic.exe
mors-avx2.exe
mors-bmi2.exe
mors-avx2+bmi2.exe
```

工具與測試 executable 同樣帶有 architecture suffix。

`make all-versions` 依序建置四種 Release architecture variant；recursive make 繼承 GNU Make jobserver，因此：

```text
make -j4 all-versions
```

仍可讓每個 variant 內部使用最多四個 parallel jobs，而不讓四個完整建置同時競爭資源。

所有 C++ translation unit 使用：

```text
-std=c++23
-Wall
-Wextra
-Wpedantic
-Wconversion
-Wshadow
-MMD
-MP
```

並固定以 UTF-8 作為 source 與 execution character set。

Release：

```text
-O3
-DNDEBUG
```

預設啟用：

```text
-flto=auto
```

可用 `LTO=0` 關閉。

Windows Release executable 靜態連結 GCC / MinGW runtime：

```text
-static
-static-libgcc
-static-libstdc++
-Wl,--gc-sections
```

避免發行 binary 額外依賴 `libstdc++-6.dll`、`libgcc_s_seh-1.dll` 或 `libwinpthread-1.dll`；正常 Windows system DLL 不受影響。

Debug：

```text
-Og
-g3
```

不使用 LTO。

Sanitize：

```text
-O1
-g3
-fsanitize=address
-fsanitize=undefined
-fno-omit-frame-pointer
```

同樣不使用 LTO。

目前 ISA variant：

```text
ARCH=generic
    無 optional ISA requirement
    MORS_MAGIC

ARCH=avx2
    -mavx2
    MORS_DUAL_HQ

ARCH=bmi2
    -mbmi2
    MORS_PEXT

ARCH=avx2+bmi2
    -mavx2 -mbmi2
    MORS_PEXT
```

因此目前不存在舊設計中的 `ARCH=hybrid` 或 `make native`。

`generic` 使用 Magic sliding attacks；`avx2` 使用 Dual Hyperbola Quintessence；`bmi2` 與 `avx2+bmi2` 使用 PEXT sliding attacks。

Makefile 會拒絕未知的 `ARCH`、`CONFIG` 或非 `0/1` 的 `LTO`，避免靜默產生錯誤建置組態。

## Engine 外部介面草案

```cpp
namespace mors::engine {

class Engine final {
public:
    explicit Engine(Config config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] auto set_position(PositionRequest request)
        -> std::expected<void, Error>;
    [[nodiscard]] auto start(SearchRequest request) -> SearchHandle;
    void stop() noexcept;
    void wait() noexcept;
    void new_game();

    [[nodiscard]] auto perft(int depth) const -> std::uint64_t;
};

} // namespace mors::engine
```

`SearchHandle`/callback 傳遞結構化的 iteration、info 與 best-move 事件；UCI 字串只在 `protocol/uci` 產生。這讓未來加入 GUI、library API 或測試驅動器時，不必侵入搜尋核心。

## 實作順序

1. **Foundation**：`src/Makefile`、warnings/sanitizers、`support`、核心型別與 bitboard。
2. **Correctness core**：FEN、Position、make/unmake、attacks、movegen、perft；先通過標準 perft suite。
3. **Usable engine**：Engine facade、UCI parser/session、單執行緒 iterative deepening、基本時間管理。
4. **Search strength**：TT、qsearch、move picker、history 與 pruning；每次調整都跑 correctness + benchmark。
5. **Parallelism**：持久 worker pool、Lazy SMP、停止/ponder 狀態機，再做 NUMA。
6. **Evaluation**：先穩定 evaluator contract，再接 NNUE accumulator、network loader 與 SIMD variants。
7. **Optional systems**：Syzygy、tuning、PGO、跨平台發行建置。

## 授權與原創性

MORS 的程式碼必須獨立撰寫，不複製、移植或衍生其他西洋棋引擎的原始碼。外部演算法、論文或測試資料若有採用，必須確認授權相容性並在專案文件中標示來源。

- License：GNU Affero General Public License v3.0 or later（`AGPL-3.0-or-later`）
- Author：Theodore Magnus Øen
- Copyright：`Copyright (C) 2026 Theodore Magnus Øen`

所有新 `.hpp` 與 `.cpp` 使用以下簡短標頭，完整授權文字放在專案根目錄的 `LICENSE`：

```cpp
// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later
```
