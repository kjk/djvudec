# Thread safety

## Contract

| Phase | Thread safety |
|-------|----------------|
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

## What `djvu_doc_open` mutates (single-threaded phase)

At open time the library:

- Parses DIRM / page table (read-only `doc->data` thereafter).
- Preloads **INFO** for every page (`has_info`, dimensions, rotation).
- Initializes the scaler bilinear lookup table (`djvu_scaler_init`).
- Decodes and caches **IW44** BG44/FG44 per page (`pages[i].iw_bg/iw_fg`).
- Decodes and caches shared **Djbz** dictionaries (`jb2_dicts[]`, inline dedup,
  `pages[i].jb2_dict` borrows).

These caches are read-only during render/text/annotation access.

## Per-call behavior (concurrent-safe paths)

- **Render** (`djvu_page_render`): decodes Sjbz mask per call into a temporary
  `jb2_image`, composites using cached IW44/dict data, returns a new
  `djvu_image`. No persistent state written back to `djvu_doc`.
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
| `scaler.c` `s_interp[]` | Was lazy-init on first scale (data race) | `djvu_scaler_init()` at doc open |
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

`djvudec_thread` opens the file once, then runs `-nops` random operations
(default 256) across `-cpu` worker threads (default `processor_count / 2`).
Each op picks a random page and one of: render, text, text zones, links.
Exits 0 if all operations succeed, 1 otherwise.

Run under Thread Sanitizer (clang `-fsanitize=thread`) for deeper validation.