# host (pseudo cases, made by the runner)

Not a disk module: the runner turns some host records (the screenshots and audio captures the guest asks for, docs/schema.md §5.1, CONTRACT §13) into pseudo cases, so they are judged and fingerprinted like guest cases (runner/amitorture/cases.py). They are not counted in the per-entry and per-module case totals.

| case | intent | rule | expected | a failure usually means |
|---|---|---|---|---|
| `shot_<tag>` | the picture of an acknowledged screenshot, on cycle-exact entries only (elsewhere the frame timing is not exact, so screenshots are kept but not compared) | the same machine, disk and seed give the same frame | always pass; the fingerprint is the PNG's sha256, so a baseline/check compares the picture | (a `check` regression: the picture changed) |
| `tone_<tag>` | the frequency of a tone the guest played (Paula `aud0`-`aud3`, sound boards), measured in the captured audio, on every entry that captured audio | the expected frequency the guest gives in the audio marks (`expect_hz`, `freq_mhz`, or clock, period and samples) and its tolerance `tol_pct` | pass within the tolerance, on the expected output channel (`side`) if given; the fingerprint is the expected frequency plus the deviation rounded to the tolerance, so it is stable on every core | fail: the wrong period or sample rate reaches the output, the wrong output channel, or a silent channel; untestable: no audio in the segment, or a tone whose length is not what the guest played (an emulator running slower than real time stretches the capture) |
