# Syzygy integration

- `vendor/fathom/` contains unmodified upstream sources at commit
  `c9c6fef0dddc05d2e242c183acf5833149ab676d`, including the MIT license.
  The `.c` extension is retained for updates; Make explicitly uses `-x c++`
  and `-std=c++23`. There is no C99 compilation step.
- `syzygy.cpp` adapts MORS bitboards, en passant and promotion encodings.
  Every root move is matched against MORS legal moves; incomplete results are
  rejected. Eligibility excludes castling rights and uses the minimum of the
  configured piece limit and Fathom's discovered maximum.
- Root DTZ probing includes the halfmove clock and repeated reversible history.
  Best-ranked moves form the root candidate set. Normal PVS chooses among them.
  Missing DTZ falls back to WDL only with a zero halfmove clock when rule50 is
  enabled. No probe result means ordinary search.
- Main-search WDL probing runs after terminal/draw detection, outside root and
  excluded-move searches. With rule50 enabled, the halfmove clock must be zero.
  A win/loss supplies a lower/upper bound; a draw is exact. Only bounds that
  actually cut the window return immediately. Qsearch does not probe WDL.
- Scores use MORS's existing tablebase band, with ply normalization; DTZ is not
  presented as distance to mate. Root reports preserve a searched mate score,
  otherwise use the tablebase result. Proven draws report `wdl 0 1000 0`.
  Restricted root results are not stored as unrestricted TT results.
- If stopped before depth one, a successful root probe still supplies a legal
  tablebase-approved fallback. `tbhits` counts main-worker successful probes.
- Fathom mappings are process-global. Initialize/unload only while **all**
  workers are idle. UCI rejects mutations during search and clears TT on changes.
  Embedded callers must serialize backend lifecycle across search pools.

## Reproducible validation

The normal `test-run` includes data-independent lifecycle/score regression tests.
For external tables and independent python-chess verification:

```text
make -C src -j8 CONFIG=release ARCH=avx2+bmi2 syzygy-check build
python tools/test-syzygy.py --engine build/release-avx2+bmi2/mors-avx2+bmi2.exe --probe build/release-avx2+bmi2/mors-syzygy-check.exe --tables D:\syzygy5 --epd E:\books-master\books-master\endgames.epd --report build/syzygy-validation.json
```

Requires `python-chess`. The script scans every EPD entry for material and legal
position validity, derives legal five-man successors from six-man samples,
compares WDL and root outcomes/moves with python-chess, and exercises UCI with
one and four workers. It also checks disabling/reloading, a WDL-only directory,
castling rejection, Chess960, rule50 switching and a one-node early stop.
It does **not** search every EPD entry or measure playing-strength gain.
A small WDL-only fixture is copied under the report directory.

The supplied EPD has 157,846 legal positions, all with 6–23 pieces, so none can
be directly probed with a five-man tablebase. The deterministic corpus derives
3,297 distinct five-man positions and adds 600 edge/random positions. Original
EPD search coverage uses all 36 six-man positions plus two per other material
count, in both thread configurations (140 searches), alongside 120 direct
endgame searches. See the generated JSON for counts and failures.
