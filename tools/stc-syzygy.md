# Syzygy integration: accepted for commit

The user accepted this change after the following interim STC result.
The 276-game sample shows no statistically significant strength difference;
it does not establish non-regression or a playing-strength gain.

## User-reported STC snapshot

| Item | Result |
| --- | --- |
| Engines | syzygy-dev vs pre-syzygy-base |
| Time control | 10+0.1 |
| Threads / Hash per engine | 1 / 64 MiB |
| Openings | UHO_Lichess_4852_v1.epd |
| Concurrency | 14 |
| SPRT | Disabled |
| Tablebases | D:\syzygy5 for dev only; no tournament-level TB adjudication |
| Games | 276 |
| Wins / losses / draws | 68 / 69 / 139 |
| Points | 137.5 (49.82%) |
| Elo | -1.26 +/- 19.29 |
| Normalized Elo | -2.68 +/- 40.99 |
| LOS | 44.91% |
| Ptnml(0-2) | [1, 28, 80, 29, 0] |

This is an interim snapshot, not a completed 20,000-game test. Committing the
implementation does not stop or restart the user's match.

## Provenance and functional validation

The base was reconstructed from `41f6d5c667ed6a1df8308742ac8194b7eea4448c`,
with the pre-existing 256 MiB default Hash change preserved in both builds.
That unrelated default-setting change is excluded from this commit.
Both engines used GCC 15.2.0, C++23, release AVX2+BMI2 and LTO, with the same
embedded network. Local binary hashes are in `build/syzygy-stc/provenance.json`.
Fathom upstream is `c9c6fef0dddc05d2e242c183acf5833149ab676d`.

Release and generic debug regression suites passed. Independent python-chess
checks passed for 3,683 WDL probes, 3,798 root probes and 260 single/multi-worker
searches. The supplied EPD contains 157,846 legal positions with 6–23 pieces;
3,297 legal five-man successors were derived for direct tablebase checks.
See `src/syzygy/README.md` for the reproducible external-data test.

GCC 15.2 AVX2 debug has a NUMA stack-alignment crash independently reproduced
without Fathom. Generic debug and release passed; this unrelated issue remains.
