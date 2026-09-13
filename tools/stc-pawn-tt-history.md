# Pawn / TT move history experiment: rejected

The uncommitted experiment was withdrawn after its STC reached the lower
SPRT boundary. Search, position hashing, tests and forward futility behavior
were restored to the pre-experiment implementation at
c12f16d3ed2ec0948b7ad2d28063f7171b2730eb. Pre-existing changes to the
256 MiB default Hash setting were preserved and are outside this experiment.

## Tested change

- Worker-private pawn history, incremental pawn key, and quiet history training.
- Pawn history in move ordering, SEE, forward futility, LMP and LMR.
- Worker-private TT move history adjusting the single singular-extension
  verification threshold, including its effect on multi-cut.
- Per-move forward futility decisions replacing the quiet-prefix skip.

These changes were tested together; the result cannot attribute the loss to
one component.

## User-reported STC result

| Setting | Value |
| --- | --- |
| Engines | pawn-tt-dev vs pre-pawn-tt-base |
| Time control | 10+0.1 |
| Threads / Hash | 1 / 64 MiB per engine |
| Openings | UHO_Lichess_4852_v1.epd |
| Games | 3528 |
| Wins / losses / draws | 820 / 917 / 1791 |
| Points | 1715.5 (48.63%) |
| Elo | -9.55 +/- 6.19 |
| Normalized Elo | -17.72 +/- 11.46 |
| LOS | 0.12% |
| Draw ratio | 51.08% |
| Pairs ratio | 0.82 |
| Ptnml(0-2) | [34, 440, 901, 367, 22] |
| WL/DD ratio | 0.83 |
| SPRT hypotheses | [0.00, 5.00], normalized |
| LLR / boundaries | -2.95 / [-2.94, 2.94] |
| Decision | Lower boundary crossed; accept H0, reject the patch |

The pre-patch base was reconstructed with the same compiler options, network,
and pre-existing TT configuration as dev. Local binaries, source provenance,
and the withdrawn patch remain under build/pawn-tt-history (ignored by Git).

No experiment implementation commit existed, so restoring its source changes
produced no code diff against HEAD. This record documents the failed trial and
withdrawal without committing unrelated work.
