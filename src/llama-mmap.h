#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <cstdio>

struct llama_file;
struct llama_mmap;
struct llama_mlock;

using llama_files  = std::vector<std::unique_ptr<llama_file>>;
using llama_mmaps  = std::vector<std::unique_ptr<llama_mmap>>;
using llama_mlocks = std::vector<std::unique_ptr<llama_mlock>>;

struct llama_file {
    llama_file(const char * fname, const char * mode, bool use_direct_io = false);
    ~llama_file();

    size_t tell() const;
    size_t size() const;

    int file_id() const; // fileno overload

    void seek(size_t offset, int whence) const;

    void read_raw(void * ptr, size_t len) const;
    void read_raw_at(void * ptr, size_t len, size_t offset) const;
    void read_aligned_chunk(size_t offset, void * dest, size_t size) const;
    uint32_t read_u32() const;

    void write_raw(const void * ptr, size_t len) const;
    void write_u32(uint32_t val) const;

    size_t read_alignment() const;
private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

struct llama_mmap {
    llama_mmap(const llama_mmap &) = delete;
    llama_mmap(struct llama_file * file, size_t prefetch = (size_t) -1, bool numa = false);
    ~llama_mmap();

    size_t size() const;
    void * addr() const;

    void unmap_fragment(size_t first, size_t last);

    static const bool SUPPORTED;

private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

// Memory mapping modes for session caches
enum class llama_mmap_mode {
    READ_ONLY,      // Read-only mapping (PROT_READ, MAP_SHARED)
    READ_WRITE,     // Read-write persistent mapping (PROT_READ|PROT_WRITE, MAP_SHARED)
    COPY_ON_WRITE,  // Copy-on-write (PROT_READ|PROT_WRITE, MAP_PRIVATE or anonymous)
};

// Read-write memory mapping for session caches
struct llama_mmap_rw {
    llama_mmap_rw(const llama_mmap_rw &) = delete;

    // Create a new file or open existing for read-write mmap
    // The file will be extended to min_size if smaller
    llama_mmap_rw(const char * filepath, size_t min_size);

    // Map an existing file for read-only access
    llama_mmap_rw(struct llama_file * file);

    // Map an existing file with copy-on-write semantics (MAP_PRIVATE)
    // If cow=true: writes modify memory only, NOT the file
    // If cow=false: same as read-only constructor
    llama_mmap_rw(struct llama_file * file, bool cow);

    ~llama_mmap_rw();

    size_t size() const;
    void * addr() const;

    // Sync changes to disk (for read-write maps only, no-op for COW)
    void sync();

    llama_mmap_mode mode() const;

    static const bool SUPPORTED;

private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

struct llama_mlock {
    llama_mlock();
    ~llama_mlock();

    void init(void * ptr);
    void grow_to(size_t target_size);

    static const bool SUPPORTED;

private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

size_t llama_path_max();
