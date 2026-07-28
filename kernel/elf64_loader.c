#include "elf64_loader.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    ELF64_HEADER_SIZE = 64,
    ELF64_PROGRAM_HEADER_SIZE = 56,
    ELF64_CLASS = 2,
    ELF64_DATA_LITTLE_ENDIAN = 1,
    ELF64_VERSION_CURRENT = 1,
    ELF64_TYPE_EXECUTABLE = 2,
    ELF64_MACHINE_X86_64 = 62,
    ELF64_PT_LOAD = 1,
    ELF64_PT_DYNAMIC = 2,
    ELF64_PT_INTERP = 3,
    ELF64_PT_TLS = 7,
    ELF64_PAGE_SIZE = 4096,
};

static uint16_t read_u16_le(const uint8_t* bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64_le(const uint8_t* bytes) {
    return (uint64_t)read_u32_le(bytes) |
           ((uint64_t)read_u32_le(bytes + 4) << 32);
}

static void clear_bytes(void* destination, size_t count) {
    uint8_t* bytes = (uint8_t*)destination;
    for (size_t index = 0; index < count; index++) {
        bytes[index] = 0;
    }
}

static void copy_bytes(void* destination, const void* source, size_t count) {
    uint8_t* out = (uint8_t*)destination;
    const uint8_t* in = (const uint8_t*)source;
    for (size_t index = 0; index < count; index++) {
        out[index] = in[index];
    }
}

static bool range_fits_size(uint64_t offset, uint64_t length,
                            size_t container_size) {
    const uint64_t size = (uint64_t)container_size;
    return offset <= size && length <= size - offset;
}

static bool is_power_of_two(uint64_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static bool virtual_ranges_overlap(uint64_t first_start, uint64_t first_end,
                                   uint64_t second_start,
                                   uint64_t second_end) {
    return first_start < second_end && second_start < first_end;
}

static bool host_ranges_overlap(const void* first, size_t first_size,
                                const void* second, size_t second_size) {
    if (first_size == 0 || second_size == 0) return false;

    const uintptr_t first_start = (uintptr_t)first;
    const uintptr_t second_start = (uintptr_t)second;
    if (first_start > UINTPTR_MAX - first_size ||
        second_start > UINTPTR_MAX - second_size) {
        return true;
    }

    const uintptr_t first_end = first_start + first_size;
    const uintptr_t second_end = second_start + second_size;
    return first_start < second_end && second_start < first_end;
}

static enum elf64_load_result inspect_load_segment(
    const uint8_t* program_header,
    size_t image_size,
    struct elf64_image_plan* plan,
    bool* entry_is_executable) {
    const uint32_t flags = read_u32_le(program_header + 4);
    const uint64_t file_offset = read_u64_le(program_header + 8);
    const uint64_t virtual_address = read_u64_le(program_header + 16);
    const uint64_t file_size = read_u64_le(program_header + 32);
    const uint64_t memory_size = read_u64_le(program_header + 40);
    const uint64_t alignment = read_u64_le(program_header + 48);

    if ((flags & ~(uint32_t)(ELF64_SEGMENT_READ |
                             ELF64_SEGMENT_WRITE |
                             ELF64_SEGMENT_EXECUTE)) != 0 ||
        memory_size == 0 || file_size > memory_size) {
        return ELF64_LOAD_INVALID_SEGMENT;
    }

    if ((flags & ELF64_SEGMENT_WRITE) != 0 &&
        (flags & ELF64_SEGMENT_EXECUTE) != 0) {
        return ELF64_LOAD_WRITABLE_CODE;
    }

    if (!range_fits_size(file_offset, file_size, image_size)) {
        return ELF64_LOAD_SEGMENT_OUT_OF_RANGE;
    }

    if (virtual_address < ELF64_LOADER_MIN_USER_VADDR ||
        virtual_address > ELF64_LOADER_MAX_USER_VADDR ||
        memory_size - 1u >
            ELF64_LOADER_MAX_USER_VADDR - virtual_address) {
        return ELF64_LOAD_SEGMENT_OUT_OF_RANGE;
    }

    if (alignment > 1u) {
        if (!is_power_of_two(alignment) ||
            (virtual_address & (alignment - 1u)) !=
                (file_offset & (alignment - 1u))) {
            return ELF64_LOAD_INVALID_SEGMENT;
        }
    }

    if (plan->segment_count >= ELF64_LOADER_MAX_LOAD_SEGMENTS) {
        return ELF64_LOAD_TOO_MANY_SEGMENTS;
    }

    const uint64_t virtual_end = virtual_address + memory_size;
    const uint64_t virtual_page_start =
        virtual_address & ~(uint64_t)(ELF64_PAGE_SIZE - 1u);
    const uint64_t virtual_page_end =
        (virtual_end + ELF64_PAGE_SIZE - 1u) &
        ~(uint64_t)(ELF64_PAGE_SIZE - 1u);
    for (size_t index = 0; index < plan->segment_count; index++) {
        const struct elf64_load_segment* existing = &plan->segments[index];
        const uint64_t existing_end =
            existing->virtual_address + existing->memory_size;
        if (virtual_ranges_overlap(virtual_address, virtual_end,
                                   existing->virtual_address,
                                   existing_end)) {
            return ELF64_LOAD_SEGMENT_OVERLAP;
        }

        /*
         * x86 permissions apply to complete 4 KiB pages. Byte-disjoint
         * writable and executable segments sharing one page would otherwise
         * become writable code when mapped, so reject that ELF at inspection
         * time instead of cataloging an image the runtime cannot launch.
         */
        const uint64_t existing_page_start =
            existing->virtual_address &
            ~(uint64_t)(ELF64_PAGE_SIZE - 1u);
        const uint64_t existing_page_end =
            (existing_end + ELF64_PAGE_SIZE - 1u) &
            ~(uint64_t)(ELF64_PAGE_SIZE - 1u);
        const uint32_t combined_flags = flags | existing->flags;
        if (virtual_ranges_overlap(
                virtual_page_start, virtual_page_end,
                existing_page_start, existing_page_end) &&
            (combined_flags & ELF64_SEGMENT_WRITE) != 0 &&
            (combined_flags & ELF64_SEGMENT_EXECUTE) != 0) {
            return ELF64_LOAD_WRITABLE_CODE;
        }
    }

    struct elf64_load_segment* segment =
        &plan->segments[plan->segment_count++];
    segment->virtual_address = virtual_address;
    segment->memory_size = memory_size;
    segment->file_size = file_size;
    segment->file_offset = file_offset;
    segment->alignment = alignment;
    segment->flags = flags;

    if (plan->virtual_base == UINT64_MAX ||
        virtual_address < plan->virtual_base) {
        plan->virtual_base = virtual_address;
    }
    if (virtual_end > plan->virtual_end) {
        plan->virtual_end = virtual_end;
    }

    /*
     * The entry must point at bytes that actually came from the executable,
     * rather than zero-filled memory at the end of an executable segment.
     */
    if ((flags & ELF64_SEGMENT_EXECUTE) != 0 &&
        plan->entry_point >= virtual_address &&
        plan->entry_point - virtual_address < file_size) {
        *entry_is_executable = true;
    }

    return ELF64_LOAD_OK;
}

enum elf64_load_result elf64_inspect(const void* image,
                                     size_t image_size,
                                     struct elf64_image_plan* out_plan) {
    if (image == NULL || out_plan == NULL) {
        return ELF64_LOAD_INVALID_ARGUMENT;
    }
    if (image_size < ELF64_HEADER_SIZE) {
        return ELF64_LOAD_TRUNCATED;
    }

    const uint8_t* bytes = (const uint8_t*)image;
    if (bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F') {
        return ELF64_LOAD_BAD_MAGIC;
    }
    if (bytes[4] != ELF64_CLASS ||
        bytes[5] != ELF64_DATA_LITTLE_ENDIAN ||
        bytes[6] != ELF64_VERSION_CURRENT ||
        read_u32_le(bytes + 20) != ELF64_VERSION_CURRENT ||
        read_u16_le(bytes + 16) != ELF64_TYPE_EXECUTABLE ||
        read_u16_le(bytes + 52) != ELF64_HEADER_SIZE ||
        read_u16_le(bytes + 54) != ELF64_PROGRAM_HEADER_SIZE) {
        return ELF64_LOAD_UNSUPPORTED_FORMAT;
    }
    if (read_u16_le(bytes + 18) != ELF64_MACHINE_X86_64) {
        return ELF64_LOAD_UNSUPPORTED_ARCHITECTURE;
    }

    const uint64_t program_offset = read_u64_le(bytes + 32);
    const uint16_t program_count = read_u16_le(bytes + 56);
    if (program_count == 0 ||
        program_count > ELF64_LOADER_MAX_PROGRAM_HEADERS) {
        return ELF64_LOAD_TOO_MANY_HEADERS;
    }
    if (program_offset < ELF64_HEADER_SIZE ||
        !range_fits_size(
            program_offset,
            (uint64_t)program_count * ELF64_PROGRAM_HEADER_SIZE,
            image_size)) {
        return ELF64_LOAD_TRUNCATED;
    }

    struct elf64_image_plan plan;
    clear_bytes(&plan, sizeof(plan));
    plan.entry_point = read_u64_le(bytes + 24);
    plan.virtual_base = UINT64_MAX;

    bool entry_is_executable = false;
    for (uint16_t index = 0; index < program_count; index++) {
        const uint8_t* program_header =
            bytes + (size_t)program_offset +
            (size_t)index * ELF64_PROGRAM_HEADER_SIZE;
        const uint32_t type = read_u32_le(program_header);

        if (type == ELF64_PT_DYNAMIC ||
            type == ELF64_PT_INTERP ||
            type == ELF64_PT_TLS) {
            return ELF64_LOAD_UNSUPPORTED_FEATURE;
        }
        if (type != ELF64_PT_LOAD) continue;

        enum elf64_load_result result =
            inspect_load_segment(program_header, image_size, &plan,
                                 &entry_is_executable);
        if (result != ELF64_LOAD_OK) return result;
    }

    if (plan.segment_count == 0) {
        return ELF64_LOAD_INVALID_SEGMENT;
    }
    if (!entry_is_executable) {
        return ELF64_LOAD_INVALID_ENTRY_POINT;
    }

    const uint64_t span = plan.virtual_end - plan.virtual_base;
    if (span == 0 || span > ELF64_LOADER_MAX_IMAGE_SPAN ||
        span > (uint64_t)SIZE_MAX) {
        return ELF64_LOAD_IMAGE_TOO_LARGE;
    }
    plan.image_span = (size_t)span;

    copy_bytes(out_plan, &plan, sizeof(plan));
    return ELF64_LOAD_OK;
}

enum elf64_load_result elf64_load_contiguous(
    const void* image,
    size_t image_size,
    void* destination,
    size_t destination_size,
    struct elf64_image_plan* out_plan) {
    if (image == NULL || destination == NULL) {
        return ELF64_LOAD_INVALID_ARGUMENT;
    }

    struct elf64_image_plan plan;
    enum elf64_load_result result =
        elf64_inspect(image, image_size, &plan);
    if (result != ELF64_LOAD_OK) return result;

    if (destination_size < plan.image_span) {
        return ELF64_LOAD_DESTINATION_TOO_SMALL;
    }
    if (host_ranges_overlap(image, image_size,
                            destination, plan.image_span)) {
        return ELF64_LOAD_BUFFER_OVERLAP;
    }

    uint8_t* loaded = (uint8_t*)destination;
    const uint8_t* source = (const uint8_t*)image;
    clear_bytes(loaded, plan.image_span);

    for (size_t index = 0; index < plan.segment_count; index++) {
        const struct elf64_load_segment* segment = &plan.segments[index];
        const size_t destination_offset =
            (size_t)(segment->virtual_address - plan.virtual_base);
        copy_bytes(loaded + destination_offset,
                   source + (size_t)segment->file_offset,
                   (size_t)segment->file_size);
    }

    if (out_plan != NULL) {
        copy_bytes(out_plan, &plan, sizeof(plan));
    }
    return ELF64_LOAD_OK;
}

const char* elf64_load_result_text(enum elf64_load_result result) {
    switch (result) {
        case ELF64_LOAD_OK: return "ok";
        case ELF64_LOAD_INVALID_ARGUMENT: return "invalid argument";
        case ELF64_LOAD_TRUNCATED: return "truncated ELF image";
        case ELF64_LOAD_BAD_MAGIC: return "not an ELF image";
        case ELF64_LOAD_UNSUPPORTED_FORMAT:
            return "unsupported ELF format";
        case ELF64_LOAD_UNSUPPORTED_ARCHITECTURE:
            return "ELF is not x86-64";
        case ELF64_LOAD_UNSUPPORTED_FEATURE:
            return "ELF uses an unsupported runtime feature";
        case ELF64_LOAD_TOO_MANY_HEADERS:
            return "too many ELF program headers";
        case ELF64_LOAD_TOO_MANY_SEGMENTS:
            return "too many loadable ELF segments";
        case ELF64_LOAD_INVALID_SEGMENT:
            return "invalid ELF load segment";
        case ELF64_LOAD_SEGMENT_OUT_OF_RANGE:
            return "ELF load segment is out of range";
        case ELF64_LOAD_SEGMENT_OVERLAP:
            return "ELF load segments overlap";
        case ELF64_LOAD_WRITABLE_CODE:
            return "ELF contains writable executable memory";
        case ELF64_LOAD_INVALID_ENTRY_POINT:
            return "ELF entry point is not executable file data";
        case ELF64_LOAD_IMAGE_TOO_LARGE:
            return "ELF virtual image is too large";
        case ELF64_LOAD_DESTINATION_TOO_SMALL:
            return "ELF destination buffer is too small";
        case ELF64_LOAD_BUFFER_OVERLAP:
            return "ELF source and destination overlap";
        default: return "unknown ELF loader result";
    }
}
