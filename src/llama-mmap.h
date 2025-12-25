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

// Read-write memory mapping for session caches
struct llama_mmap_rw {
    llama_mmap_rw(const llama_mmap_rw &) = delete;

    // Create a new file or open existing for read-write mmap
    // If the file exists and is smaller than min_size, it will be extended
    // If the file doesn't exist, it will be created with min_size
    llama_mmap_rw(const char * filepath, size_t min_size);

    // Map an existing file for read-only access
    llama_mmap_rw(struct llama_file * file);

    ~llama_mmap_rw();

    size_t size() const;
    void * addr() const;

    // Sync changes to disk (for read-write maps)
    void sync();

    // Resize the mapping (only for read-write maps created with filepath)
    void resize(size_t new_size);

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
