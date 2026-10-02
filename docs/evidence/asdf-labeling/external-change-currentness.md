# Canonical ASDF external-change currentness (#83)

Measured on 2026-10-03 with the production benchmark, Ninja/MSVC Debug,
MSVC 19.44.35228.0, Windows build 26300, AMD64 Family 26 Model 68,
16 logical CPUs, local workspace filesystem.

Command (substitute the case, count, and a distinct output directory):

```powershell
build/ninja-msvc-debug/tools/spectiary_asdf_labeling_production_benchmark.exe `
  --case explicit-unicode --operation values-rewrite-1 `
  --directory build/issue83-benchmark --sample-count 100000
```

| Case | Samples | Published bytes | Values save wall time | Encoded roster reused |
| --- | ---: | ---: | ---: | --- |
| source-index | 100,000 | 5,076 | 11.215 ms | Not applicable |
| explicit-unicode | 100,000 | 277,998 | 134.386 ms | Yes |
| explicit-unicode | 1,000,000 | 2,763,885 | 1,318.683 ms | Yes |

These are single-run complete save timings including currentness checking,
compression, staging and replacement, not isolated hash overhead or release
latency guarantees. No before/after speedup claim is made. All saves succeeded.
The million-row Debug result remains substantial; no speculative cache or
watcher was added to hide that cost.

The added work on each successful values save is two sequential SHA-256 passes:
one over the closed staged file, one over the current target. Each uses the
existing 64 KiB buffer. The baseline is the staged file's digest and advances
only on successful publication. Conflict detection never decodes the full ASDF
or materializes another roster. Opening uses matching digests before and after
decoding/source validation; metadata saves use the existing validated reopen.

Focused regression coverage includes equal-length edits with restored mtime,
same-path replacement, deletion, rename, changes during open/staging, sticky
rejection even after original bytes return, independent metadata and values
write checks, dirty content surviving source switches, and repeated normal
publications. The store, store-property, labeling-controller and source-session
test suites passed. Final controller handoff changes also passed focused
external-conflict, ordinary-retry and metadata-reopen recovery tests.

This checks content equivalence, not file identity. Byte-identical replacements
and unobserved change-and-restore sequences are indistinguishable. The final
check-to-replace interval is not an atomic compare-and-swap; arbitrary concurrent
external writers remain outside the guarantee. See the
[ownership contract](../../reference/labeling/labeling_persistence_ownership.md).
