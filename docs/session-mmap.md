# Memory-Mapped Session Cache

This document describes the memory-mapped session cache feature, which provides efficient I/O for session state persistence.

## Overview

Memory-mapped session caches allow you to:
- Load session state without copying data into intermediate buffers
- Share session files across multiple processes (read-only)
- Pre-allocate fixed-size caches for predictable memory usage
- Use copy-on-write semantics for temporary modifications

## API Functions

### Opening Session Caches

```c
// Read-only: map existing session file
struct llama_state_mmap * llama_state_mmap_open(
    struct llama_context * ctx,
    const char * path_session);

// Read-write: create or open session file with persistence
struct llama_state_mmap * llama_state_mmap_open_rw(
    struct llama_context * ctx,
    const char * path_session);

// Read-write with explicit size pre-allocation
struct llama_state_mmap * llama_state_mmap_open_rw_sized(
    struct llama_context * ctx,
    const char * path_session,
    size_t size);

// Copy-on-write: read from file, writes go to memory (not persisted)
struct llama_state_mmap * llama_state_mmap_open_cow(
    struct llama_context * ctx,
    const char * path_session,
    size_t writable_size);  // 0 = auto-size based on context
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

// Save context state to mmap
size_t llama_state_mmap_save(
    struct llama_context * ctx,
    struct llama_state_mmap * mmap,
    const llama_token * tokens,
    size_t n_token_count);

// Sync changes to disk (read-write mode only)
void llama_state_mmap_sync(struct llama_state_mmap * mmap);

// Get mmap address and size
void * llama_state_mmap_addr(const struct llama_state_mmap * mmap);
size_t llama_state_mmap_size(const struct llama_state_mmap * mmap);

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

Creates or opens a session file for reading and writing. Changes are persisted to disk.

```c
llama_state_mmap * mmap = llama_state_mmap_open_rw(ctx, "session.bin");
llama_state_mmap_save(ctx, mmap, tokens, n_tokens);
llama_state_mmap_sync(mmap);  // ensure data is on disk
llama_state_mmap_free(mmap);
```

### Copy-on-Write Mode

Reads from an existing file but writes go to anonymous memory. The original file is never modified. This is useful for:

- Loading a base session and making temporary modifications
- Processing multiple requests from the same base session in parallel
- Testing without affecting the original file

```c
llama_state_mmap * mmap = llama_state_mmap_open_cow(ctx, "base_session.bin", 0);

// Load original state
llama_state_mmap_load(ctx, mmap, tokens, capacity, &count);

// ... process more tokens ...

// Save updated state (only in memory, not to disk)
llama_state_mmap_save(ctx, mmap, new_tokens, n_new_tokens);

llama_state_mmap_free(mmap);
// Original file unchanged
```

## Pre-allocation

For read-write mode, you can pre-allocate a fixed-size cache:

```c
// Pre-allocate 100MB for session cache
size_t cache_size = 100 * 1024 * 1024;
llama_state_mmap * mmap = llama_state_mmap_open_rw_sized(ctx, "session.bin", cache_size);
```

For copy-on-write mode, the `writable_size` parameter controls the buffer size:

```c
// 0 = auto-calculate based on context size
llama_state_mmap * mmap = llama_state_mmap_open_cow(ctx, "session.bin", 0);

// Or specify explicit size
llama_state_mmap * mmap = llama_state_mmap_open_cow(ctx, "session.bin", 50 * 1024 * 1024);
```

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
| Copy to buffer | Yes | No (direct access) |
| Resize on save | Automatic | Manual or pre-allocated |
| Multiple readers | Via file locks | Native sharing |
| Copy-on-write | No | Yes |
| Windows support | Yes | Yes |
| Memory efficiency | Lower | Higher |

## Implementation Notes

- Read-only and copy-on-write modes use `MAP_PRIVATE` or anonymous memory
- Read-write mode uses `MAP_SHARED` for persistence
- The session file format is the same as `llama_state_save_file()`
- Files can be opened with either API interchangeably
