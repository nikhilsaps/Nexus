# Host tests

`test_safe_replace.cpp` exercises `main/safe_replace.h` against an in-memory
FAT-like filesystem, with a fault injectable at every single step of the
replace sequence (backup rename, commit rename, restore rename, sidecar
removal, exclusive open, fdopen, fflush/ferror/fclose).

    cd tests && make test        # g++ or clang++, C++17, no ESP-IDF

What is covered:

- replace over an existing target, and onto a fresh name
- an existing `.nexus-bak` is **refused, never deleted** (both with and without
  a live target) - the data-loss regression this header exists to prevent
- failure of the backup rename leaves the original in place
- failure of the commit rename restores the original
- failure of commit **and** restore leaves the original in `.nexus-bak` and
  deletes nothing
- a failed sidecar cleanup blocks the next save instead of clearing the sidecar
- `nx_sr_recover()`: no-op, orphan-backup restore, ambiguous refusal, rename failure
- the save-handler commit policy (replace -> recover -> replace) after an
  interrupted save
- exclusive temp creation: collision reports `EEXIST` without touching the
  other writer's bytes; `fdopen` failure leaves no zero-byte temp
- `nx_sr_fclose_checked()` reports flush, stream-error and close failures
- path rules: traversal, `.`/`..` components, empty components, `\` and the
  FAT-hostile set, control characters, trailing dot/space components (FAT name
  aliasing), reserved sidecar suffixes (case-insensitive), length bound

`make test` also builds and runs the other `test_*.cpp` files in this
directory (`test_path_push`, `test_gzip`, `test_recovery_regression`);
`make asan` reruns them under ASan/UBSan. `test_ui_guards.py` is a separate
source-level check: `python3 test_ui_guards.py`.

Not covered here: `localserve.cpp` itself (needs `esp_http_server`); its
handlers need on-device HTTP tests; a successful firmware build alone does
not verify their behavior. No on-device HTTP requests have been run in this pass.
