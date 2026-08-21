# MORS search-distillation datagen

`mors-datagen` turns opening-book positions into BulletFormat `.data` records
that can be read directly by the MNUE P2-H32 trainer.

For every selected book position, the tool chooses a uniformly random legal
move for 1–40 plies. Each reached position is searched with a fixed node
budget. The same worker keeps its transposition table across the whole chain,
so searches of later positions can reuse entries produced earlier.

Each 32-byte record contains:

- the position in BulletFormat's side-to-move-normalised representation;
- the teacher search score converted to centipawns;
- a discrete white win/draw/loss sampled from the teacher's calibrated search
  WDL distribution, then normalised as required by BulletFormat.

Example:

```powershell
mingw32-make -C src ARCH=avx2+bmi2 CONFIG=release datagen

& "F:\MORS\build\release-avx2+bmi2\mors-datagen-avx2+bmi2.exe" `
  --book "E:\books-master\books-master\UHO_4060_v4.epd" `
  --network "F:\MORS\networks\mors-p2h32-s14400M-o3183M-c+frc.mnue" `
  --output "D:\NNUE\mors-distill-g1.data" `
  --positions 100000000 `
  --nodes 50000 `
  --extend-min 1 `
  --extend-max 40 `
  --workers 14 `
  --hash 16 `
  --seed 1
```

`--hash` is per worker. The example therefore allocates approximately 224 MB
of transposition-table memory. Existing output is replaced unless `--append`
is supplied. Append mode rejects files that do not end on a complete 32-byte
record boundary.
