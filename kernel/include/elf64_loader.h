#ifndef ELF64_LOADER_H
#define ELF64_LOADER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Nostalux Apps v1 accepts small, statically linked x86-64 ET_EXEC images.
 * The loader deliberately rejects dynamic linking, TLS, writable executable
 * segments, overlapping load ranges, and non-canonical user addresses.
 *
 * Loading copies the PT_LOAD contents into a caller-owned contiguous buffer.
 * It does not install page tables or transfer control to the image.
 */
#define ELF64_LOADER_MAX_PROGRAM_HEADERS 32u
#define ELF64_LOADER_MAX_LOAD_SEGMENTS   16u
#define ELF64_LOADER_MAX_IMAGE_SPAN      (16u * 1024u * 1024u)
#define ELF64_LOADER_MIN_USER_VADDR      0x0000010000000000ull
#define ELF64_LOADER_MAX_USER_VADDR      0x000001ffffffffffull

enum elf64_segment_flags {
    ELF64_SEGMENT_EXECUTE = 1u << 0,
    ELF64_SEGMENT_WRITE   = 1u << 1,
    ELF64_SEGMENT_READ    = 1u << 2,
};

struct elf64_load_segment {
    uint64_t virtual_address;
    uint64_t memory_size;
    uint64_t file_size;
    uint64_t file_offset;
    uint64_t alignment;
    uint32_t flags;
};

struct elf64_image_plan {
    uint64_t entry_point;
    uint64_t virtual_base;
    uint64_t virtual_end;
    size_t image_span;
    size_t segment_count;
    struct elf64_load_segment segments[ELF64_LOADER_MAX_LOAD_SEGMENTS];
};

enum elf64_load_result {
    ELF64_LOAD_OK = 0,
    ELF64_LOAD_INVALID_ARGUMENT,
    ELF64_LOAD_TRUNCATED,
    ELF64_LOAD_BAD_MAGIC,
    ELF64_LOAD_UNSUPPORTED_FORMAT,
    ELF64_LOAD_UNSUPPORTED_ARCHITECTURE,
    ELF64_LOAD_UNSUPPORTED_FEATURE,
    ELF64_LOAD_TOO_MANY_HEADERS,
    ELF64_LOAD_TOO_MANY_SEGMENTS,
    ELF64_LOAD_INVALID_SEGMENT,
    ELF64_LOAD_SEGMENT_OUT_OF_RANGE,
    ELF64_LOAD_SEGMENT_OVERLAP,
    ELF64_LOAD_WRITABLE_CODE,
    ELF64_LOAD_INVALID_ENTRY_POINT,
    ELF64_LOAD_IMAGE_TOO_LARGE,
    ELF64_LOAD_DESTINATION_TOO_SMALL,
    ELF64_LOAD_BUFFER_OVERLAP,
};

/*
 * Parses and validates an image without writing executable bytes anywhere.
 * out_plan is updated only when ELF64_LOAD_OK is returned.
 */
enum elf64_load_result elf64_inspect(const void* image,
                                     size_t image_size,
                                     struct elf64_image_plan* out_plan);

/*
 * Revalidates the image, clears the complete virtual span, copies each
 * file-backed PT_LOAD range, and leaves BSS bytes zeroed. destination must not
 * overlap image. out_plan may be NULL.
 */
enum elf64_load_result elf64_load_contiguous(
    const void* image,
    size_t image_size,
    void* destination,
    size_t destination_size,
    struct elf64_image_plan* out_plan);

const char* elf64_load_result_text(enum elf64_load_result result);

#endif /* ELF64_LOADER_H */
