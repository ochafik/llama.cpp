# Memory-Mapped Session Cache

This document describes the memory-mapped session cache feature, which provides efficient I/O for session state persistence.

## Overview

Memory-mapped session caches allow you to:
- Load session state without copying data into intermediate buffers
- Share session files across multiple processes (read-only or COW)
- Use copy-on-write (COW) semantics for parallel processing with zero-copy reads

**Important**: Session files are pre-allocated to maximum size (`llama_state_size_max()`) to avoid runtime resizing. This ensures predictable memory usage and enables true copy-on-write.

## API Functions

### Size Calculation

```c
// Get current state size (for informational purposes)
size_t llama_state_get_size(struct llama_context * ctx);

// Get maximum possible state size for this context
// Use this for pre-allocating buffers
size_t llama_state_size_max(const struct llama_context * ctx);
```

### Opening Session Caches

```c
// Read-only: map existing session file
struct llama_state_mmap * llama_state_mmap_open(
    struct llama_context * ctx,
    const char * path_session);

// Read-write: create or open session file
// File is pre-allocated to llama_state_size_max()
struct llama_state_mmap * llama_state_mmap_open_rw(
    struct llama_context * ctx,
    const char * path_session);

// Copy-on-write: map file with MAP_PRIVATE
// Reads come from file, writes go to private memory (not persisted)
struct llama_state_mmap * llama_state_mmap_open_cow(
    struct llama_context * ctx,
    const char * path_session);
```

### Using Session Caches

```c
// Load state from mmap into context
bool llama_state_mmap_load(
    struct llama_context * ctx,
    struct llama_state_mmap * mmap,
    llama_token * tokens_out,
    size_t n_token_capacity,
    size_t * n_token_count_out);

// Save context state to mmap (read-write or COW mode)
size_t llama_state_mmap_save(
    struct llama_context * ctx,
    struct llama_state_mmap * mmap,
    const llama_token * tokens,
    size_t n_token_count);

// Sync changes to disk (read-write mode only)
void llama_state_mmap_sync(struct llama_state_mmap * mmap);

// Free mmap handle
void llama_state_mmap_free(struct llama_state_mmap * mmap);

// Check platform support
bool llama_state_mmap_supported(void);
```

## Modes

### Read-Only Mode

Opens an existing session file for reading. The file cannot be modified.

```c
llama_state_mmap * mmap = llama_state_mmap_open(ctx, "session.bin");
llama_state_mmap_load(ctx, mmap, tokens, capacity, &count);
llama_state_mmap_free(mmap);
```

### Read-Write Mode

Creates or opens a session file for reading and writing. Changes are persisted to disk. Files are pre-allocated to maximum size.

```c
llama_state_mmap * mmap = llama_state_mmap_open_rw(ctx, "session.bin");
llama_state_mmap_save(ctx, mmap, tokens, n_tokens);
llama_state_mmap_sync(mmap);  // ensure data is on disk
llama_state_mmap_free(mmap);
```

### Copy-on-Write Mode

Uses `MAP_PRIVATE` (POSIX) or `FILE_MAP_COPY` (Windows) for true copy-on-write:
- **Reads**: Zero-copy from the file via page cache
- **Writes**: Lazily copied to private memory, never persisted

This is ideal for:
- Loading a base session and making temporary modifications
- Processing multiple requests from the same base session in parallel
- Testing without affecting the original file

```c
llama_state_mmap * mmap = llama_state_mmap_open_cow(ctx, "base_session.bin");

// Load original state (zero-copy read from file)
llama_state_mmap_load(ctx, mmap, tokens, capacity, &count);

// ... process more tokens ...

// Save updated state (writes to private memory, not file)
llama_state_mmap_save(ctx, mmap, new_tokens, n_new_tokens);

llama_state_mmap_free(mmap);
// Original file unchanged
```

## File Size

Session files are pre-allocated to the maximum possible size for the context:

```c
size_t max_size = llama_state_size_max(ctx);
printf("Max session file size: %.2f MB\n", max_size / 1024.0 / 1024.0);
```

This includes space for:
- Session header (magic, version, token count)
- Maximum tokens (`n_ctx * sizeof(llama_token)`)
- Maximum state (logits, embeddings, KV cache)

## Platform Support

Memory-mapped session caches are supported on:
- POSIX systems (Linux, macOS, BSD) via `mmap()`
- Windows via `CreateFileMapping()`/`MapViewOfFile()`

Check support at runtime:

```c
if (!llama_state_mmap_supported()) {
    // Fall back to regular file I/O
    llama_state_load_file(...);
}
```

## Comparison with File-Based I/O

| Feature | `llama_state_load_file` | `llama_state_mmap_*` |
|---------|------------------------|---------------------|
| Copy on read | Yes | No (zero-copy) |
| File size | Exact state size | Pre-allocated max |
| Multiple readers | Via file locks | Native sharing |
| Copy-on-write | No | Yes (MAP_PRIVATE) |
| Windows support | Yes | Yes |

## Implementation Notes

- Read-only mode uses `MAP_SHARED` with `PROT_READ`
- Read-write mode uses `MAP_SHARED` with `PROT_READ|PROT_WRITE`
- COW mode uses `MAP_PRIVATE` with `PROT_READ|PROT_WRITE`
- The session file format is identical to `llama_state_save_file()`
- Files created with mmap can be read with `llama_state_load_file()` and vice versa
