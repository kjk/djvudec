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
| `djvu_doc_drop_page_cache` / `djvu_doc_page_cache_size` | Safe concurrently with renders when lock/unlock hooks are set (cached layers are refcounted; a layer pinned by an in-flight render is freed on its last release) |
| `djvu_request_abort` | Thread-safe; aborts every in-flight render on the ctx, later renders run normally |
| `djvu_abort_request` | Thread-safe; aborts only the render(s) given that token. The token must outlive the render call it was passed to |

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

## Caching flags

Set `djvu_ctx_set_cache_per_page` before `djvu_doc_open` (default off).

| Behavior | Effect | Lock required |
|----------|--------|---------------|
| Shared **Djbz** dicts | Cached on the doc. Without lock/unlock hooks: all pre-decoded at open. With hooks: decoded lazily on first use, serialized via the hooks | No (hooks optional; with them open is cheaper) |
| `cache_per_page` | Retain page-local decoded layers (**IW44**, **Sjbz**, composited BG) on each page | Yes (`lock`/`unlock` on `djvu_ctx`) |

With `cache_per_page` off: page-local layers are decoded per use and not
retained on the doc (except the open-time chunk index and INFO preload).

Without lock/unlock hooks, shared dicts are read-only after open. With hooks,
the first use of a shared dict decodes it under the caller's lock; after that
it is read-only. When `cache_per_page` is on, the first
concurrent decode of a given page-local layer is serialized via the caller's
lock callbacks; later renders of the same page reuse the cached layer.

## What `djvu_doc_open` mutates (single-threaded phase)

At open time the library:

- Parses DIRM / page table (read-only `doc->data` thereafter).
- Preloads **INFO** for every page (`has_info`, dimensions, rotation).
- Initializes the scaler bilinear lookup table (`djvu_init` / `djvu_scaler_init`).
- Builds a chunk index at open (page-local `DJVU_PG_*` flags + unique **INCL**
  ids for shared **DJVI** includes).
- Pre-decodes shared **Djbz** dictionaries (INCL + deduped inline), but only
  when the `djvu_ctx` has no lock/unlock hooks. With hooks, they are decoded
  lazily on first use under the lock.

Cached data is read-only once decoded. First-time decodes after open (shared
dicts with hooks; page-local layers when `cache_per_page` is on) are
serialized via the lock callbacks.

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

## Aborting renders

Abort is cooperative: a render polls at checkpoints (after the mask decode,
every 64 blits in the bitonal and composite loops, at composite entry, once
per IW44 slice and around each IW44 plane's inverse transform), so a late
request may let the render complete. JB2 decode itself has no checkpoint. An
aborted IW44 decode is partial and is never stored in the page cache.
`djvu_aborted` only reports an abort on a thread that is inside a render call
on that ctx, so `djvu_doc_open` and the non-render APIs are unaffected by
earlier `djvu_request_abort` calls. An aborted render returns NULL / -1,
frees everything it allocated and leaves the page cache usable. The per-render
state lives in thread-local storage, set at the public render entry and
cleared at exit. `djvu_test -verify-abort` (`bun cmd/test-abort.ts`) checks all
of this with aborts injected from the allocator callback.

## Stress harness

```text
bun cmd/build-thread.ts
bun cmd/thread.ts deps/DjVuLibre/doc/djvu3spec.djvu
bun cmd/thread.ts -cpu 4 -nops 512 deps/DjVuLibre/doc/djvu2spec.djvu
```

`djvudec_thread` opens the file once with per-page caching and lock callbacks,
then runs `-nops` random operations (default 256) across `-cpu` worker threads
(default `processor_count / 2`). Each op picks a random page and one of: render,
text, text zones, links. Exits 0 if all operations succeed, 1 otherwise.

`bun cmd/stress-test.ts <dir | file.djvu ... | -rand N>` (`djvudec_stress`,
ASan by default) renders every page of the selected files from N threads with
per-page caching, the way SumatraPDF's render threads do.
`bun cmd/test-cache-race.ts` (`test/crash_cache_race.c`) races
`djvu_doc_drop_page_cache` against renders of the same pages (regression test
for the cached-layer refcounts).

No script builds with Thread Sanitizer yet (clang `-fsanitize=thread`); that
would be the next step for deeper validation.