# SpecForge ASDF labeling spike

This directory contains the reproducible experiments for GitHub issue #77. It
is deliberately separate from the production labeling persistence path. The
approved semantic model remains owned by issue #74.

The reference oracle is Python `asdf` 5.3.1 with `asdf-standard` 1.5.0. Install
the pinned environment into an ignored build directory:

```powershell
python -m pip install --target build/asdf-labeling-spike-python -r tools/asdf_labeling_spike/requirements.txt
$env:PYTHONPATH = (Resolve-Path build/asdf-labeling-spike-python).Path
```

Generate and validate the checked-in golden corpus:

```powershell
python tools/asdf_labeling_spike/asdf_spike.py generate --output tests/fixtures/asdf_labeling
python tools/asdf_labeling_spike/asdf_spike.py validate --fixtures tests/fixtures/asdf_labeling
```

Run the representation, wire-version, missing-value, and autosave experiments:

```powershell
python tools/asdf_labeling_spike/asdf_spike.py experiments --output build/asdf-labeling-spike-results
```

Run the second-round canonical-format and realistic-edit matrix requested by
the follow-up on issue #77:

```powershell
python tools/asdf_labeling_spike/asdf_spike.py second-round --output build/asdf-labeling-spike-results
```

This runs 96 isolated cases over 10k, 100k, and 1M samples. It compares compact
UTF-8 JSON, whole-document JSON + zlib, ASDF UCS-4/int32 blocks, and ASDF +
zlib at levels 1, 6, and 9 for ASCII/Unicode rosters and mostly-labeled/
mostly-unlabeled values. Each case measures initial write, hydration, one-label
and 1000-label edits, full atomic persistence, and (for ASDF) atomic persistence
that copies the immutable encoded roster block verbatim and re-encodes only the
values block. `foreground_edit` is only the synchronous in-memory mutation;
the separately reported persistence time is work eligible for a debounced
background autosave. The command writes `second-round-benchmark.json`.

Build the native subset with the repository's required MSVC wrapper, then run
the compressed read/write/rewrite matrix:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Configure -Preset ninja-msvc-debug-asdf-labeling-spike
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-ninja-msvc-debug.ps1 `
  -Preset ninja-msvc-debug-asdf-labeling-spike -Target specforge_asdf_labeling_spike_native
python tools/asdf_labeling_spike/asdf_spike.py interoperability `
  --fixtures tests/fixtures/asdf_labeling `
  --native build/ninja-msvc-debug-asdf-labeling-spike/tools/asdf_labeling_spike/specforge_asdf_labeling_spike_native.exe `
  --output build/asdf-labeling-spike-results/interoperability.json
```

The matrix proves Python zlib writer -> native reader, native level-6 zlib
writer -> Python reader, and Python zlib writer -> native label-only rewrite ->
both readers. The rewrite test verifies that the encoded Unicode roster block
is byte-for-byte unchanged while the values block is replaced. The native CLI
also exposes that seam directly for an already validated input document:

```powershell
build/ninja-msvc-debug-asdf-labeling-spike/tools/asdf_labeling_spike/specforge_asdf_labeling_spike_native.exe `
  rewrite-value input.asdf output.asdf 0 1
```

Both benchmark commands use a subprocess per case so reported peak RSS is not
contaminated by allocations retained by earlier cases. Generated benchmark
files are deleted after each case; the JSON evidence is retained.
