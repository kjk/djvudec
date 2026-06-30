# Thread safety

## Contract

Call `djvu_init()` once from the main thread before spawning workers or
opening documents from multiple threads. It is idempotent and also runs inside
`djvu_doc_open`, but an explicit early call avoids a data race on the scaler
lookup table if decode starts concurrently before the first `djvu_doc_open`
finishes.

| Phase | Thread safety |
|-------|----------------|
| `djvu_init` | Call once, main thread, before concurrent decode |
| `djvu_ctx_new` / `djvu_ctx_free` | Single-threaded |
| `djvu_doc_open` / `djvu_doc_close` | Single-threaded (mutates doc caches) |
| All other APIs on an open `djvu_doc` | Intended safe for concurrent read-only use |

After `djvu_doc_open` returns, multiple threads may call `djvu_page_render`,
`djvu_page_text`, `djvu_page_text_get_zones`, `djvu_page_get_links`,
`djvu_doc_outline`, `djvu_doc_page_info`, etc. on the **same** `djvu_doc`.

Each call allocates its own outputs (`djvu_image`, text buffers, zone trees,
link lists). The caller frees them via the matching destroy functions. Outputs
must not be shared across threads without external synchronization.

The in-memory file buffer passed to `djvu_doc_open` must remain valid and
unchanged until `djvu_doc_close`.

`djvu_ctx` alloc/free callbacks must be thread-safe if a shared `djvu_ctx` is
used from multiple threads (the default `malloc`/`free` are fine on Windows and
glibc).

## Caching flags (both default off)

Set before `djvu_doc_open` via `djvu_ctx_set_cache_precache_shared` and
`djvu_ctx_set_cache_per_page`.

| Flag | Effect | Lock required |
|------|--------|---------------|
| `cache_precache_shared` | Pre-decode shared **Djbz** dicts (INCL + deduped inline) at open | No |
| `cache_per_page` | Retain page-local decoded layers (**IW44**, **Sjbz**, composited BG) on each page | Yes (`lock`/`unlock` on `djvu_ctx`) |

With both off: shared dicts and page-local layers are decoded per use and not
retained on the doc (except the open-time chunk index and INFO preload).

When `cache_precache_shared` is on, shared dicts are read-only after open.
When `cache_per_page` is on, the first concurrent decode of a given page-local
layer is serialized via the caller's lock callbacks; later renders of the same
page reuse the cached layer.

## What `djvu_doc_open` mutates (single-threaded phase)

At open time the library:

- Parses DIRM / page table (read-only `doc->data` thereafter).
- Preloads **INFO** for every page (`has_info`, dimensions, rotation).
- Initializes the scaler bilinear lookup table (`djvu_init` / `djvu_scaler_init`).
- Builds a chunk index at open (page-local `DJVU_PG_*` flags + unique **INCL**
  ids for shared **DJVI** includes).
- Optionally pre-decodes shared **Djbz** dictionaries when
  `cache_precache_shared` is enabled.

Cached data is read-only during render/text/annotation access (shared dicts
always; page-local layers when `cache_per_page` is on).

## Per-call behavior (concurrent-safe paths)

- **Render** (`djvu_page_render`): uses cached or freshly decoded layer data
  depending on the caching flags; returns a new `djvu_image`. With
  `cache_per_page`, concurrent first access to a page's layers is serialized via
  the caller's lock callbacks.
- **Text** (`djvu_page_text`, `djvu_page_text_get_zones`): reads chunk bytes from
  `doc->data`, allocates fresh UTF-8 / zone tree.
- **Links** (`djvu_page_get_links`): reads ANTa/ANTz (or INCL), allocates fresh
  link array.
- **Outline** (`djvu_doc_outline`): reads NAVM once per call, allocates tree.
- **Metadata** (`djvu_doc_page_info`, `djvu_page_get_type`, page id/title):
  read-only on `doc` after open.

## Global / process-wide state

| Location | Issue | Mitigation |
|----------|-------|------------|
| `scaler.c` `s_interp[]` | Lazy-init on first scale (data race) | Call `djvu_init()` at startup; also at doc open |
| `zptable.c` / `zpcodec.c` | Const decode tables after link | Safe (read-only) |
| `getenv("DJVU_*")` in debug hooks | Not synchronized; debug only | Do not set env vars during concurrent decode |

## Former lazy-init on `djvu_doc_page_info`

`page_load_info` used to set `pages[i].has_info` on first `djvu_doc_page_info`
call. That was a write race if two threads hit the same page concurrently.
INFO is now parsed for all pages inside `djvu_doc_open`.

## Stress harness

```text
bun cmd/build_thread.ts
bun cmd/thread.ts testfiles/subset/djvu3spec.djvu
bun cmd/thread.ts -cpu 4 -nops 512 testfiles/subset/foo.djvu
```

`djvudec_thread` opens the file once with per-page caching and lock callbacks,
then runs `-nops` random operations (default 256) across `-cpu` worker threads
(default `processor_count / 2`). Each op picks a random page and one of: render,
text, text zones, links. Exits 0 if all operations succeed, 1 otherwise.

Run under Thread Sanitizer (clang `-fsanitize=thread`) for deeper validation.