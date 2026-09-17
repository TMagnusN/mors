# MORS Search 數值與 TT 契約

本文件記錄 `search.hpp/.cpp` 的實作狀態與 Search 使用的數值契約。目標是讓 NNUE、PVS、qsearch、SEE、move ordering 與 Transposition Table（TT）共用清楚的契約，而不是把所有整數都當成同一種「分數」。

## 目前實作狀態

Iterative deepening 在第一層完成後，以前一層分數為中心使用 aspiration
window。初始半窗為 16 centipawns；fail-low 只擴張 alpha，fail-high 只擴張
beta，並以 1.5 倍逐次放寬。若節點限制在重搜期間觸發，該未完成深度不會
覆蓋上一個完整結果。

時間控制使用 soft/hard 兩層 deadline：soft deadline 只在完整 iterative
deepening depth 後停止；hard deadline 每 64 nodes 輪詢並立即傳播
`VALUE_NONE`，確保 make/unmake 與增量 NNUE 狀態仍完整還原。UCI search
在背景執行，外部 `stop` 透過 atomic flag 使用相同的取消路徑。

`search.hpp/.cpp` 已提供各 worker 獨立執行的 iterative-deepening PVS：第一個著法使用完整窗口，後續著法先做 null-window probe，只有改善 alpha 且尚未 fail-high 時才完整重搜。第一版同時接入 check-aware qsearch、增量 NNUE、TT probe/store、TT move ordering、mate-distance normalization、repetition、50-move、insufficient-material、PV 與 hard node limit。

目前已加入 TT raw static-eval cache、non-PV reverse futility/null-move pruning、
跨 UCI go 保留的 worker-private butterfly quiet history、killer/countermove ordering、
main-search LMP/LMR、forward futility、singular extension 與 Lazy SMP，以及
Reckless-style qsearch threshold SEE 與 noisy late-move pruning。
目前已實作 worker-private continuation history 與 noisy history，參與走法排序與
主搜尋 SEE 門檻；correction history 尚未實作。

qsearch 已整合 TT probe/store，以 `DEPTH_QS` 保存結果；只有 non-PV 節點可依
有效 depth/bound 截斷，PV 節點仍搜尋以保留主變例。Raw static eval 可從 TT 重用，
stand-pat fail-high 存為 lower bound；完成搜尋後依原始窗口保存 upper/lower/exact。
Mate 與 rule-50 分數轉換沿用主搜尋契約，中斷的子搜尋不發布部分結果。
合法的安靜 TT move 若不在 noisy list 中，仍可使用其分數與 raw eval，但不加入
非將軍 qsearch 的走法。TT 排序優先級不能當作 SEE 通過的證據。

## 分數領域總覽

| 名稱 | 用途 | 單位 | 可寫入 TT score |
|---|---|---:|---:|
| `Value` | NNUE、alpha-beta/PVS、qsearch、mate/TB | 約 1 centipawn | 是 |
| `SeeValue` | 單一目標格上的交換結果與 SEE threshold | pawn = 100 | 否 |
| `MoveScore` | MovePicker 排序優先級 | 無物理單位 | 否 |
| `HistoryScore` | quiet/capture/continuation history | 無物理單位 | 否 |
| `CorrectionScore` | 修正 raw static evaluation 的學習量 | fixed-point 權重 | 否 |
| `Depth` | 剩餘搜尋工作量 | ply | TT 另有 depth 欄位 |

底層都可以暫時使用 `std::int32_t`，但 API、變數名與 helper 必須保持領域分離。某個整數能隱式轉成 `Value`，不代表語意上允許這樣做。

## `Value` 的視角與基本單位

所有 Search `Value` 一律從「目前 side to move」視角表示：

- 正值：目前走子方較好。
- 負值：目前走子方較差。
- `VALUE_DRAW == 0`：和棋。
- 搜尋子節點後以 negamax 反號：`score = -child_score`。

NNUE raw output、corrected static evaluation、PVS 回傳值與 root move 的 search score 都遵守此視角。任何白方固定視角的值都必須在進入 Search 前轉換。

MORS 第一版約定普通 `Value` 的一個單位約等於一 centipawn，pawn 約為 100。這是引擎內部與 UCI 顯示的尺度約定，不是勝率的數學保證；日後可以在 protocol/WDL 層校準，但不可偷偷改變 Search 內部的符號或 mate 區間。

## `Value` 數值格線

MORS 採用 Reckless 式的 decisive/TB 預留概念，但依自己的容量建立常數：

```cpp
inline constexpr int MAX_PLY = 240;

inline constexpr Value VALUE_DRAW               =      0;
inline constexpr Value VALUE_EVAL_MAX           = 31'518;
inline constexpr Value VALUE_TB_WIN_IN_MAX_PLY  = 31'519;
inline constexpr Value VALUE_TB                 = 31'759;
inline constexpr Value VALUE_MATE_IN_MAX_PLY    = 31'760;
inline constexpr Value VALUE_MATE               = 32'000;
inline constexpr Value VALUE_INFINITE           = 32'001;
inline constexpr Value VALUE_NONE               = 32'002;
```

負值區間完全對稱。正半軸的意義為：

```text
0 .. 31518       ordinary evaluation / search value
31519 .. 31759   reserved decisive / future tablebase value
31760 .. 32000   mate value
32001            search-window sentinel only
32002            missing-value sentinel only
```

`MAX_PLY = 240` 與未來 Fathom/Syzygy 的常見最大 ply 契約一致，也讓目前 256 層 NNUE state capacity 留下 16 層 guard。Search 到達上限時必須安全停止延伸，不能讓 stack、PV table 或 NNUE state 走出容量。

需要提供並測試以下 predicates：

```cpp
is_valid_value(v)   // -VALUE_MATE <= v && v <= VALUE_MATE
is_decisive(v)      // abs(v) >= VALUE_TB_WIN_IN_MAX_PLY
is_mate_value(v)    // abs(v) >= VALUE_MATE_IN_MAX_PLY
is_win(v)           // v >= VALUE_TB_WIN_IN_MAX_PLY
is_loss(v)          // v <= -VALUE_TB_WIN_IN_MAX_PLY
```

重要限制：

- `VALUE_NONE` 不是分數，不可反號、加減、比較優劣或回傳成搜尋結果。
- `VALUE_INFINITE` 只初始化 alpha-beta window，不可由 NNUE/Search 回傳，也不可寫入 TT。
- TT 中尚未 `value_from_tt()` 的 decisive value 可能因 ply normalization 落入另一個表面區段，不可先拿它判斷 mate/TB 類型。
- 普通評估即使極端，也必須 clamp 在 `[-VALUE_EVAL_MAX, VALUE_EVAL_MAX]`。

`nnue::Worker` 會 clamp 到 ordinary-evaluation 邊界
`[-VALUE_EVAL_MAX, VALUE_EVAL_MAX]`（目前為 `[-31'518, 31'518]`），避免極端
網路輸出落入 tablebase 或 mate score 區間。

## Raw static evaluation 與 corrected evaluation

`raw_static_eval` 是 NNUE 對當前 position 的直接結果：

- 型別為 `Value`。
- side-to-move relative。
- 必須位於 ordinary evaluation 區間。
- 可保存在 TT 的 `static_eval` 欄位。
- 它不是 Search 對節點的最終判斷，不能直接當 exact TT score。

日後加入 correction history 後：

```text
corrected_eval = clamp_eval(raw_static_eval + scaled_correction)
```

`corrected_eval` 是當次搜尋 context 的估值，可供 pruning、stand pat 或 improving 判斷；TT 仍保存 `raw_static_eval`，不能保存已套用 history/correction 的結果，否則不同搜尋世代會互相污染。

在 check 中沒有合法的 stand-pat 靜態局面。第一版可以令 `static_eval == VALUE_NONE`，或只為其他用途計算，但 qsearch 絕不能在 check node 用它直接 cutoff。

和棋規則第一版固定回傳零。Reckless 以少量 node-dependent jitter 避免重複循環是可測的 heuristic，但 MORS 第一版不採用：TT、repetition 與測試先保持真正的 `draw == 0`。未來若加入 contempt，也只能在 root/presentation policy 明確施加。

## `Value` 的證據等級

相同型別、相同單位，不表示同樣可靠。每個 `Value` 還必須有明確 provenance：

| 等級 | 例子 | 可以如何使用 |
|---|---|---|
| terminal/exact | checkmate、stalemate、完整 PV search 落在 window 內 | 可作 exact result |
| lower bound | fail-high、可信的 TT lower entry | 只證明真實值不低於它 |
| upper bound | fail-low、可信的 TT upper entry | 只證明真實值不高於它 |
| heuristic estimate | raw/corrected eval、futility estimate | 只供 ordering/pruning decision |
| sentinel | `VALUE_NONE`、`VALUE_INFINITE` | 控制流程，不是局面價值 |

Bound 是關於「這次搜尋證明了什麼」，不是 score 正負號。正分可以是 upper bound，負分也可以是 lower bound。Raw NNUE、corrected eval、futility value 與 history 推出的估計永遠不能直接標成 `BOUND_EXACT`。

## Mate、decisive value 與 ply

Mate helper：

```cpp
mate_in(ply)  =  VALUE_MATE - ply;
mated_in(ply) = -VALUE_MATE + ply;
```

例如目前節點下一手將死，子節點在 `ply + 1` 回傳 `mated_in(ply + 1)`，negamax 反號後自然得到較近的正 mate。這保證搜尋偏好更快將死、延後被將死。

終局回傳：

- 無合法著且被將軍：`mated_in(ply)`。
- 無合法著且未被將軍：`VALUE_DRAW`。
- repetition、50-move 與 insufficient material：`VALUE_DRAW`。

### TT normalization

Mate/decisive score 含有相對 root 的 ply。同一 position 可從不同路徑、不同 ply 抵達，寫入 TT 前必須轉為 position-relative，讀出後再轉回目前 root-relative：

```cpp
[[nodiscard]] constexpr Value value_to_tt(Value value, int ply) noexcept {
    assert(is_valid_value(value));

    if (is_win(value))
        return value + ply;
    if (is_loss(value))
        return value - ply;
    return value;
}

[[nodiscard]] constexpr Value value_from_tt(
    Value value,
    int ply,
    std::uint16_t halfmove_clock
) noexcept {
    if (value == VALUE_NONE)
        return VALUE_NONE;

    value = rule50_safe_decisive_value(value, halfmove_clock);

    if (is_win(value))
        return value - ply;
    if (is_loss(value))
        return value + ply;
    return value;
}
```

這裡使用 decisive threshold，而不只 mate threshold，讓未來 tablebase distance values 也有一致的路徑語意。Hobbes 的檔案切分值得參考，但 TT 轉換的正負方向不能按函式名照搬，必須以 MORS 的「store 加 root ply、probe 減目前 ply」round-trip 測試決定。

### 50-move 與 decisive TT score

Zobrist position key 通常不包含 halfmove clock，因此相同棋盤可能在不同剩餘 50-move 距離 probe 到同一筆 decisive score。Reckless 在 TT decode 時會檢查 stored mate/TB distance 是否超過剩餘 reversible plies。

MORS 第一版至少要採以下其中一種安全策略：

1. `value_from_tt()` 同時取得 halfmove clock，將不再可靠的 decisive score 降到 ordinary 邊界；或
2. 拒絕該 score 作 cutoff，但仍使用 TT move 與 raw eval。

不能永久保留只看 `score + ply` 的簡化版本，否則接近 50-move draw 時可能從 TT 宣稱不存在的 forced mate。

## PVS / alpha-beta 的 `Value` 契約

核心介面概念：

```cpp
Value search(Position&, Depth depth, Value alpha, Value beta, int ply);
```

window 採整數 half-open 語意 `[alpha, beta)`，永遠滿足 `alpha < beta`：

- PV/full window：可容納一段未知分數。
- Non-PV/null window：`beta == alpha + 1`，只回答能否超過 alpha。
- 搜尋採 fail-soft；fail-low/high 時回傳實際找到的 bound，不把結果硬 clamp 成 alpha 或 beta。

Negamax child window：

```cpp
// PV 的第一個 move，或需要完整重搜
score = -search(child, new_depth, -beta, -alpha, ply + 1);

// 後續 move 先做 null-window probe
score = -search(child, new_depth, -alpha - 1, -alpha, ply + 1);

// probe 改善 alpha 但尚未達 beta，PV node 再完整重搜
if (score > alpha && score < beta)
    score = -search(child, new_depth, -beta, -alpha, ply + 1);
```

必須在 move loop 前保存有效工作 window 的 `original_alpha`。節點完成後的 TT bound：

```cpp
if (best_value >= beta)                 BOUND_LOWER; // fail-high
else if (best_value <= original_alpha)  BOUND_UPPER; // fail-low
else                                    BOUND_EXACT;
```

`BOUND_LOWER` 表示真實值至少為 stored value；`BOUND_UPPER` 表示真實值至多為 stored value。`BOUND_EXACT` 只能在分數確實落入原 window 時保存，不能因 reduced/null-window search 回傳漂亮分數就標 exact。

Mate-distance pruning 先縮緊合法 window：

```cpp
alpha = std::max(alpha, mated_in(ply));
beta  = std::min(beta,  mate_in(ply + 1));
if (alpha >= beta)
    return alpha;
```

Search 絕不能對 `VALUE_NONE` 執行 negamax 反號。任何可能 miss 的來源，例如 TT probe，都必須先檢查 sentinel。

### 常見 Search 變數的精確含義

| 變數 | 領域 | 含義 |
|---|---|---|
| `alpha` | `Value` | 目前已證明的下界；節點只關心能否改善它 |
| `beta` | `Value` | cutoff 邊界，不是「預期分數」 |
| `original_alpha` | `Value` | move loop 前的工作 window 下界，供 TT bound 判定 |
| `best_value` | `Value` | 已搜尋 moves 的最佳 fail-soft 結果，證據等級由 window 決定 |
| `tt_value` | `Value` | 已從 TT position-relative 轉回目前 root ply 的值，仍受 bound/depth 限制 |
| `raw_static_eval` | `Value` | NNUE 原始 ordinary estimate，可寫入 TT static-eval 欄位 |
| `corrected_eval` | `Value` | 套用 correction 後的 ordinary estimate，不可回寫成 raw eval |
| `estimated_value` | `Value` | 結合可信資訊後供 pruning 的估計，不是 exact search result |
| `futility_value` | `Value` | static estimate 加一個 `Value` margin 的樂觀上界 |
| `probcut_beta` | `Value` | 比 beta 更高的驗證門檻，不是 move-order score |
| `singular_beta` | `Value` | 排除 TT move 後驗證 singularity 的搜尋門檻 |
| `aspiration_center` | `Value` | 前一輪 root result/average，作下一輪 window 中心 |
| `aspiration_delta` | `Value` magnitude | 非負 window 半徑；不是 position score |
| `root_move.score` | `Value` | 該 root move 本輪真正的搜尋結果 |
| `see_threshold` | `SeeValue` | 允許的局部 material 交換門檻 |
| `move_score` | `MoveScore` | 排序鍵，完全不進 alpha-beta 算術 |
| `history` | `HistoryScore` | 有界學習統計，只影響 ordering/reduction/pruning policy |

Search margin 若以 centipawn 尺度調整，可以沿用 `Value` 的單位，但它是「差值」而非 absolute position value。命名應包含 `_margin`、`_delta` 或 `_beta`，避免來源不明的裸 `score`。

## Qsearch 的 `Value` 契約

Qsearch 與主搜尋回傳完全相同的 `Value` 領域：

- 非 check node 以 ordinary static evaluation 作 stand pat。
- `stand_pat >= beta` 是 lower-bound cutoff。
- `stand_pat > alpha` 時提升 alpha。
- check node 禁止 stand pat，必須搜尋所有合法 evasions。
- 沒有合法 evasion 時回傳 `mated_in(ply)`。
- qsearch TT depth 使用 `DEPTH_QS`，不可冒充正深度結果。
- delta/futility margin 是 `Value`；SEE threshold 是 `SeeValue`，兩者需由明確 policy 產生，不能意外混算。

## SEE 的 score 是什麼

SEE（Static Exchange Evaluation）回答：「若雙方在同一目標格依序交換，這一步的淨物質結果是否至少達到 threshold？」它是局部交換模型，不是 position evaluation，也不考慮 NNUE 的位置價值。

第一版定義獨立名稱：

```cpp
using SeeValue = std::int32_t;

PAWN   = 100;
KNIGHT = 320;
BISHOP = 330;
ROOK   = 500;
QUEEN  = 900;
```

King 不是可交換的 material value。King capture/recapture 必須以合法性與 attacked-square 規則處理，而不是給 KING 一個普通棋子價格。

建議 hot-path API：

```cpp
bool see_ge(const Position&, Move, SeeValue threshold) noexcept;
```

threshold 語意：

- `see_ge(move, 0)`：交換至少不虧 material。
- `see_ge(move, -100)`：最多容忍虧一兵。
- `see_ge(move, 100)`：至少淨賺一兵。

完整 `see_value()` 可以只供 unit test、debug 或少量 ordering 使用；pruning hot path 優先直接做 threshold SEE，像 Reckless/Hobbes 一樣允許早退。

特殊著法必須定義清楚：

- promotion 初始收益包含 `captured + promoted_piece - pawn`。
- en passant 的被吃兵不在 target square，occupancy 必須特別移除後再算 x-ray。
- pinned attacker、slider x-ray 與 king recapture 後目標格是否安全都會改變交換合法性。
- quiet move 通常沒有 capture gain，但 quiet promotion 仍有 `promoted_piece - pawn`。

`SeeValue` 採 pawn=100 只是讓 threshold 直觀；它不等於 NNUE `Value`。禁止把 SEE 結果加到 static eval、寫入 TT，或直接當 PVS 回傳值。

Hobbes 為 ordering 與 pruning 各自調了一套 SEE piece values；Reckless 則使用單一 tuned material table。MORS 第一版選擇單表：先減少自由度並把合法性做好，只有 self-play/tuning 證明雙表有效時才拆成 `SeeType::Ordering/Pruning`。

## Move ordering score

`MoveScore` 只是「先搜尋誰」的排序鍵，數字越大越優先，沒有 centipawn 意義。它應使用彼此不重疊的 category bands：

```text
TT move
good captures / promotions
killers or countermove
quiet history
bad captures
```

每個 band 內可結合 MVV/LVA、SEE、capture history、quiet history；最後結果要 saturate，避免 history 累積溢位跨越 category。以下操作一律錯誤：

- `static_eval + move_score`
- 用 `move_score >= beta` 作 cutoff
- 把 `MoveScore` 寫入 `TTData::value`

Root move 的 `score`、`previous_score`、`average_score` 是真正的 Search `Value`；它們雖然也參與 root 排序，但不屬於 `MoveScore`。

## History 與 correction score

`HistoryScore` 是有界、無單位的學習統計。Reckless 與 Hobbes 都使用 gravity 類更新，MORS 契約為：

```text
next = current + bonus - current * abs(bonus) / limit
```

bonus 先 clamp，運算使用足夠寬的中間型別，最後再寫回 bounded storage。Quiet history、capture history、continuation history 是不同表，不能因底層都是整數就混用。

History 可用於 MovePicker band 內排序、LMR reduction 調整及 history pruning，但不可直接變成 position `Value`。

目前 `QuietHistory` 維持 `[side][from][to]` 三維索引，`int16_t` 儲存共
16 KiB／worker，上限仍為 8192，cutoff bonus/malus 與讀取規則不變。
尚未加入起點／終點受攻擊狀態維度。

Quiet、continuation 與 noisy history 位於各 worker 私有且持久的 `ThreadData`，連續 UCI `go` 保留學習。
`ucinewgame` 透過 `SearchThreadPool::clear()` 重建所有 ThreadData，再清空 TT；
OS threads 保留。搜尋中禁止 clear／resize，改變 thread count 會重建各 worker
的歷史。`Clear Hash` 僅清 TT；獨立同步 `search()` 每次使用新的 ThreadData。
Killer、countermove、PV、repetition keys 與 NNUE 搜尋堆疊仍屬於每次 job 的 Context。

`CorrectionScore` 同樣是有界學習值，但唯一用途是透過明確 divisor/grain 轉成 `Value` residual，再修正 raw static evaluation。更新的學習目標是可信 search score 與 raw/corrected eval 的差；mate/TB、in-check、不可靠 bound 等節點不可污染 correction。套用後必須 clamp 回 ordinary evaluation 區間，TT 始終保存 raw eval。

## Depth、ply 與 seldepth

這三者都不是 score：

- `Depth`：距離 horizon 的剩餘工作量，以 ply 為單位，可因 reduction/extension 改變。
- `ply`：目前節點離 root 的距離，供 mate encoding、stack 與 repetition 使用。
- `seldepth`：本次搜尋實際到達的最大 ply，只用於資訊輸出。

`depth <= DEPTH_QS` 進入 qsearch；`DEPTH_UNSEARCHED` 只表示 TT evaluation-only entry。Reduction、extension 與 singular extension 都必須以 `Depth` 表示。

## UCI 顯示邊界

只有 protocol formatter 可以把內部 `Value` 轉成人類輸出：

- ordinary value：第一版顯示 `score cp N`。
- mate value：轉成帶正負號的 mate moves，不能顯示成巨大 cp。
- future TB decisive value：由 tablebase/WDL policy 顯示，不偽裝成 mate。
- `VALUE_NONE` / `VALUE_INFINITE`：永不輸出。

搜尋核心不包含字串格式化，也不為 UCI mate moves 改變內部以 ply 表示的距離。

## Hash 容量上限

UCI 與 TT resize 共用 `MAX_TT_SIZE_MB = 2,147,483,647` MiB，也就是 2 PiB 減 1 MiB。
這個上限可由 signed 32-bit GUI／工具安全解析。索引仍使用 Zobrist key 的高 bits，
每個 cluster 仍為 32 bytes。實際配置仍受可用記憶體、
作業系統位址空間與配置器限制。
預設 Hash 為 256 MiB，超限輸入在配置前拒絕。

## 目前 TT 版面

`TTCluster` 固定為 32 bytes、32-byte aligned，一個 cluster 保存三筆資料：

```text
TTCluster (32 bytes)
|-- atomic payload[0]  8 bytes
|-- atomic payload[1]  8 bytes
|-- atomic payload[2]  8 bytes
`-- atomic signatures  8 bytes (3 x uint16_t + 1 guard lane)
```

每筆 payload 包含：

```text
Move        16 bits
value       16 bits
staticEval  16 bits
depth        8 bits
flags        8 bits  (bound + PV + generation)
```

設計不變量：

- key 低 16 bits 只作 verification signature。
- key 高 48 bits 經 multiply-high reduction 產生 cluster index。
- index 與 signature 不重用相同 key bits。
- signature 0 表示 empty、0xFFFF 表示 writer busy；對應 key signature 會映射到相鄰值。
- 三個 signature 以 SWAR 一次比較，命中後仍逐槽精確確認。
- `depth == 0` 表示空 entry；`DEPTH_UNSEARCHED == -2` 仍可編碼成有效 entry。
- TT 允許只保存 raw `static_eval`、`BOUND_NONE`、`VALUE_NONE` 的 evaluation-only entry。

## TT probe 契約

TT hit 不等於一定可以 cutoff。即使 entry 深度或 bound 不足，仍可利用 TT move、raw static evaluation 與 PV 資訊。

```cpp
TTProbe tt = table.probe(position.key());

if (tt.hit) {
    const Value tt_value = value_from_tt(tt.data.value, ply);
    const bool depth_ok = tt.data.depth >= depth;
    const bool bound_ok =
           tt.data.bound == BOUND_EXACT
        || (tt.data.bound == BOUND_LOWER && tt_value >= beta)
        || (tt.data.bound == BOUND_UPPER && tt_value <= alpha);

    if (!pv_node
        && tt_value != VALUE_NONE
        && depth_ok
        && bound_ok) {
        return tt_value;
    }
}
```

第一版只在 Non-PV node 使用一般 TT cutoff。PV、singular search、tablebase 與 search exclusion 需要更嚴格條件，加入相應功能時再擴充。

### TT move 驗證

16-bit signature 只能降低碰撞，不能證明完整 key 相同。因此 TT move 永遠是不可信輸入，不可未驗證就執行。

```cpp
if (!tt_move.is_none()
    && position.is_pseudo_legal(tt_move)
    && position.is_legal(tt_move)) {
    move_picker.set_tt_move(tt_move);
}
```

在單步合法性 API 完成前，可用 legal move list 確認 TT move 存在。若非空 TT move 驗證失敗，第一版把整筆 probe 視為 miss，同時拒絕 score 與 static evaluation。Move 為空的 evaluation-only entry 不受此規則影響。

## TT store 契約

```cpp
tt.writer.write({
    .move        = best_move,
    .value       = value_to_tt(best_value, ply),
    .static_eval = raw_static_eval,
    .depth       = depth,
    .bound       = bound,
    .pv          = pv_node
});
```

規則：

- `BOUND_LOWER`：fail-high，真實值至少為儲存值。
- `BOUND_UPPER`：fail-low，真實值至多為儲存值。
- `BOUND_EXACT`：分數落在原始 alpha-beta window 內。
- `static_eval` 保存 raw NNUE evaluation，不保存 corrected evaluation。
- `VALUE_NONE` 只能搭配 `BOUND_NONE`，不得執行 score cutoff。
- `VALUE_INFINITE` 永遠不得寫入。
- 新資料沒有 move 且 signature 相同時，保留原有 TT move。
- 較淺的同 generation、non-exact 資料不應覆蓋明顯更深資料。
- evaluation-only entry 使用 `VALUE_NONE`、`DEPTH_UNSEARCHED` 與 `BOUND_NONE`。

## Replacement、generation 與 prefetch

Probe miss 時優先選空 slot；cluster 已滿時，以最低 quality entry replacement：

```text
quality = decodedDepth - AGE_PENALTY * relativeAge
```

目前 generation 使用 5 bits，每 32 次 root search 循環：

1. 建立或調整 Hash option 時呼叫 `resize()`。
2. `ucinewgame` 或明確清除 hash 時呼叫 `clear()`。
3. 每次新的 root search 呼叫一次 `new_search()`。
4. `hashfull()` 只計算 current-generation entries。

Generation wrap 是正常行為；relative age 必須使用模 32 計算。

得到 child position key 後、進入遞迴搜尋前可預取 TT cluster：

```cpp
position.do_move(move, state);
table.prefetch(position.key());
```

Prefetch 只是 cache hint，不可改變正確性，也不應為取得 key 重算完整 Zobrist。

## `search.hpp/.cpp` 邊界

公開檔名固定使用 `search.hpp` 與 `search.cpp`，不建立名為 `SearchWorker` 的公開類別。公開層只暴露穩定的 request/result/limits API。

同步公開 `search()` 建立一個私有 `SearchCoordinator` 與 main `SearchWorker`。
UCI 則持有公開的 `SearchThreadPool`：所有 OS thread 跨 `go` 常駐並以 barrier
同步開始同一個 Lazy SMP job。每個 thread 擁有持久的 `ThreadData`，每個 job
建立自己的 `SearchWorker`、root copy 與 Context。Pool 每個 job 只推進一次 TT
generation，只有 main worker 能呼叫 iteration callback 並決定最終結果。

```text
public search()
`-- SearchCoordinator
    |-- new_search() once
    |-- main-worker iteration callback
    `-- SearchWorker
        |-- private root Position copy
        `-- per-job Context

UCI SearchThreadPool
|-- persistent main thread
|   |-- persistent ThreadData -> QuietHistory
|   `-- SearchWorker -> per-job Context -> ThreadData&
`-- persistent helper threads -> private ThreadData + per-job Contexts
```

每個 job 的暫時搜尋狀態放在 `search.cpp` 私有 `Context`，學習資料透過參考連到持久 ThreadData：

```text
search.cpp private Context
|-- TranspositionTable&
|-- Network& / nnue::Worker
|-- SearchStack[MAX_PLY + guard]
|-- repetition keys
|-- PV table / root moves
|-- ThreadData& -> worker-private quiet/continuation/noisy history
|-- node/depth counters
`-- limits / stop state
```

`Context` 與 `ThreadData` 都是實作細節，不出現在 engine/protocol 公開 header。
每個 persistent thread 的 ThreadData 跨 job 保留；每個 job 建立 SearchWorker、
獨立 Context 並複製 root Position。所有 worker 從 depth 1 進行 iterative deepening，並依 worker index 使用
略微不同的 aspiration delta；所有 worker 只共享停止狀態、全域 node budget、唯讀
network 與支援並行 probe/write 的 TT。
Main 完成後會發布停止，pool 等全部 helper 回到 idle 才送出 completion callback。

UCI 與 `SearchThreadPool::resize()` 共用 `MAX_SEARCH_THREADS = 22,528`，
預設為 1；實際建立數量受作業系統與可用記憶體限制。超限輸入在建立 worker 前拒絕。

`NumaPolicy` 預設為 `auto`。可用 CPU 跨 NUMA node 或 Windows processor group
時先綁定 worker，再由 worker 初始化私有 history。使用中的 node 各有一份
唯讀 NNUE 複本；Context 的 accumulator／PV 等仍在 worker 內建立。
單 node 且單 group 的 auto，以及 none 模式，使用 OS 排程與原始共享 NNUE。
CPU 與記憶體配置介面位於 [platform](../platform/README.md)。

`clear()` 透過同步 maintenance job 在各 worker 上建立替代 history，全部成功後
才交換；不重啟 OS threads。改變 Threads／NumaPolicy 時先建立完整的新 pool，
成功後才退役舊 pool。EvalFile 的所有節點複本先準備完成，才切換來源網路。
控制操作由呼叫者序列化；搜尋中拒絕重配置。TT 配置與 probe/store 契約不變。

目前檔案責任與後續拆分方向：

- `chess/types.hpp`：底層 `Value`/`Depth` 型別與全域保留數值區間。
- `search/score.hpp`：mate、decisive predicates、TT normalization、eval clamp helper。
- `search/see.hpp/.cpp`：`SeeValue`、piece values 與 `see_ge()`。
- `search/search.hpp/.cpp`：iterative deepening、PVS、qsearch 與私有 Context。
- 走法排序目前位於 `search.cpp`，使用完整列表排序；`search/move_picker.hpp/.cpp` 與 staged generation 為後續拆分方向，尚未實作。
- History 的有界更新目前位於 `search.cpp`；獨立 `search/history.hpp` 與 correction history 尚未實作。

## 並行邊界

TT 的 `probe()`、`TTWriter::write()`、`hashfull()` 與 `new_search()` 可由多個搜尋
worker 並行呼叫。每個 64-bit payload 使用 lock-free atomic；writer 先以 signature
lane 的 busy sentinel 取得 slot，再發布完整 payload 與最終 signature。Reader 會在
讀取 payload 前後驗證 signature lane，避免接受正在替換的 entry。

`TTWriter` 仍保存 cluster 指標與 slot，`resize()` / `clear()` 仍會使它失效。因此這
兩個操作不是搜尋期操作：engine 必須先發出 stop 並等待 ThreadPool 回到 idle，才能
清空或替換 table storage。ThreadPool 本身也只允許在 idle 狀態 resize。一次 root
search 只由 job orchestration layer 呼叫一次 `new_search()`；worker 不應各自增加
generation。
Node-limited job 以原子 compare/exchange 共用精確 budget；一般 time/depth job 則讓
每個 worker 發布自己的 local node counter，UCI iteration 與最終結果回報其總和。

## 參考結論

從 Reckless 採納的是：

- `MATE -> TB -> ordinary eval -> draw` 的保留格線。
- `MAX_PLY = 240` 與 decisive score 的 TT ply normalization。
- ordinary evaluation 必須 clamp 在 decisive band 之外。
- 50-move clock 會影響 TT decisive score 是否仍可信。
- threshold SEE、pin/x-ray/king recapture 的完整合法性交換模型。
- bounded gravity history 與 raw/corrected eval 分離。

從 Hobbes 採納的是：

- `score.rs`、`see.rs`、history、correction 的模組責任分離。
- SEE ordering/pruning 可各自 tuning，但不是第一版必要複雜度。
- `MAX_PLY + guard` 的 stack 配置與清楚的 protocol formatting 邊界。

只學習演算法與不變量，不複製兩者程式碼或參數。它們的 tuned piece values、history weights、pruning margins 都不適合直接成為 MORS 常數。

## 實作順序與必測不變量

第一階段：

1. 加入 score constants/helpers，修正 NNUE clamp。
2. 為 mate、TT conversion、sentinel 與 numeric bands 寫 unit tests。
3. 實作 `see_ge()` 與特殊著法測試。
4. 實作最小 iterative deepening + PVS + qsearch。
5. 接入 TT probe/store 與 TT move ordering。
6. 加入 repetition、50-move、mate-distance pruning。
7. 再加入 MovePicker、history、LMR 與其他 pruning，每次以固定 bench 與對局驗證。

最低測試集合：

- 所有 score bands 的 ordering `static_assert`。
- NNUE 永不回傳 decisive、mate 或 sentinel value。
- `VALUE_NONE` 經 TT miss/probe 保持 sentinel，且從不被反號。
- 不同 ply 的 `value_to_tt()` / `value_from_tt()` 正負 mate round-trip。
- halfmove clock 接近 100 時不誤用過遠 decisive TT score。
- checkmate、stalemate、repetition 與 50-move 的回傳值。
- PVS null-window fail-high 後完整重搜，結果與 full-window 基準一致。
- TT upper/lower/exact bound 的 cutoff 條件。
- SEE threshold 的贏子、等價交換、虧子、promotion、en passant、pin、x-ray 與 king safety。
- `MoveScore` category 不因 history 最大/最小值而跨 band 或溢位。

先讓這些語意穩定，再增加 pruning。Search 強度策略可以調，數值契約不能隨 heuristic 漂移。


## 主搜尋 SEE 實驗

主搜尋在落子前，以深度、走法類型及 history 決定 SEE 門檻。此實驗依使用者
要求，先採用 Reckless 的門檻係數，尚未經 MORS 對局調參驗證。
安靜步使用原有 quiet history 加前 1、2 層 continuation history；非安靜步使用
以 moving piece、destination、captured type 索引的 noisy history（吃過路兵按
PAWN 記錄，無吃子升變按 NO_PIECE_TYPE 記錄）。這是 MORS 的索引與更新方式，
未移植 Reckless 的 threat-conditioned history。

新 history 與 quiet history 同樣為 worker 私有、跨 job 保留並隨 clear/resize 重建。
安靜步 beta cutoff 更新 quiet/continuation history，非安靜步 cutoff 更新 noisy
history，已搜尋但失敗的對應走法扣分；跳過的走法不訓練。Continuation 不跨空步。
目前安靜步排序使用 butterfly 加前 1、2 層 continuation history，非安靜步排序在材料分數上加入 noisy history。TT、killer、countermove 優先級與 SEE 好／壞分組保留；FP、LMP、LMR 仍使用原有 butterfly history。

根節點、excluded search、尚未搜尋任何走法、best score 仍屬 loss，以及被將軍時的
安靜應將步不做主搜尋 SEE 剪枝。`SearchLimits::use_see_pruning` 預設開啟；關閉
僅停用主搜尋 SEE，不影響 qsearch SEE 與 history 學習，便於固定條件比較。
`SearchStats` 的 `see_prunes`、`see_quiet_prunes`、`see_noisy_prunes` 記錄剪枝數。


本次 dev/base 的使用者回報：STC `10+0.1`、1 thread、64 MB、
`UHO_4060_v4.epd`，280 局為 89 勝／54 負／137 和，得分率 56.25%，
fastchess 顯示 Elo +43.66 ±23.25、LOS 99.99%。這是初步局數結果，
不代表已完成 SPRT 或 LTC 驗證。

制式對局腳本保存在 `tools/run-stc.ps1` 與 `tools/run-ltc.ps1`，沿用使用者
本機路徑與裁決設定。STC 為 `10+0.1`、64 MB；LTC 為 `60+0.6`、256 MB；
兩者均為每引擎 1 thread、並行 14、交換先後手。執行前需備妥 fastchess
目錄內的 `dev.exe`、`base.exe`。兩套腳本共用 `dev-vs-base.pgn` 輸出名稱，
不同測試的輸出須自行保留或更名。
