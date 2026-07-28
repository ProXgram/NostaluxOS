#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_abi.h"
#include "app_catalog.h"
#include "app_manifest.h"
#include "app_process.h"
#include "elf64_loader.h"

enum {
    TEST_IMAGE_SIZE = 0x300,
    TEST_PROGRAM_OFFSET = 64,
    TEST_PROGRAM_HEADER_SIZE = 56,
    TEST_CODE_OFFSET = 0x100,
    TEST_DATA_OFFSET = 0x200,
};

#define TEST_IMAGE_VADDR (ELF64_LOADER_MIN_USER_VADDR + 0x400000ull)
#define TEST_CODE_VADDR  (TEST_IMAGE_VADDR + 0x100ull)
#define TEST_DATA_VADDR  (TEST_IMAGE_VADDR + 0x1000ull)

static void put_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t* bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void put_u64(uint8_t* bytes, uint64_t value) {
    put_u32(bytes, (uint32_t)value);
    put_u32(bytes + 4, (uint32_t)(value >> 32));
}

static uint8_t* program_header(uint8_t* image, size_t index) {
    return image + TEST_PROGRAM_OFFSET +
           index * TEST_PROGRAM_HEADER_SIZE;
}

static void set_program_header(uint8_t* header,
                               uint32_t type,
                               uint32_t flags,
                               uint64_t file_offset,
                               uint64_t virtual_address,
                               uint64_t file_size,
                               uint64_t memory_size,
                               uint64_t alignment) {
    memset(header, 0, TEST_PROGRAM_HEADER_SIZE);
    put_u32(header, type);
    put_u32(header + 4, flags);
    put_u64(header + 8, file_offset);
    put_u64(header + 16, virtual_address);
    put_u64(header + 32, file_size);
    put_u64(header + 40, memory_size);
    put_u64(header + 48, alignment);
}

static void make_valid_elf(uint8_t image[TEST_IMAGE_SIZE]) {
    memset(image, 0, TEST_IMAGE_SIZE);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2; /* ELFCLASS64 */
    image[5] = 1; /* ELFDATA2LSB */
    image[6] = 1; /* EV_CURRENT */
    put_u16(image + 16, 2);  /* ET_EXEC */
    put_u16(image + 18, 62); /* EM_X86_64 */
    put_u32(image + 20, 1);
    put_u64(image + 24, TEST_CODE_VADDR);
    put_u64(image + 32, TEST_PROGRAM_OFFSET);
    put_u16(image + 52, 64);
    put_u16(image + 54, TEST_PROGRAM_HEADER_SIZE);
    put_u16(image + 56, 2);

    set_program_header(program_header(image, 0),
                       1, ELF64_SEGMENT_READ | ELF64_SEGMENT_EXECUTE,
                       TEST_CODE_OFFSET, TEST_CODE_VADDR, 4, 8, 0x100);
    set_program_header(program_header(image, 1),
                       1, ELF64_SEGMENT_READ | ELF64_SEGMENT_WRITE,
                       TEST_DATA_OFFSET, TEST_DATA_VADDR, 3, 16, 0x100);

    image[TEST_CODE_OFFSET + 0] = 0x31; /* xor eax,eax */
    image[TEST_CODE_OFFSET + 1] = 0xc0;
    image[TEST_CODE_OFFSET + 2] = 0xc3; /* ret */
    image[TEST_CODE_OFFSET + 3] = 0x90;
    image[TEST_DATA_OFFSET + 0] = 'A';
    image[TEST_DATA_OFFSET + 1] = 'P';
    image[TEST_DATA_OFFSET + 2] = 'P';
}

static struct app_manifest valid_manifest(void) {
    const struct app_manifest manifest = {
        .manifest_version = NOSTALUX_APP_MANIFEST_VERSION,
        .abi_version = NOSTALUX_APP_ABI_VERSION,
        .capabilities = APP_CAPABILITY_LOG |
                        APP_CAPABILITY_TIME |
                        APP_CAPABILITY_WINDOW,
        .id = "test-app",
        .display_name = "Test App",
        .executable = "test-app.elf",
        .description = "Synthetic Apps v1 test application",
    };
    return manifest;
}

static void test_valid_inspect_and_load(void) {
    uint8_t image[TEST_IMAGE_SIZE];
    uint8_t destination[0x1000];
    struct elf64_image_plan plan;
    make_valid_elf(image);
    memset(destination, 0xa5, sizeof(destination));

    assert(elf64_inspect(image, sizeof(image), &plan) == ELF64_LOAD_OK);
    assert(plan.entry_point == TEST_CODE_VADDR);
    assert(plan.virtual_base == TEST_CODE_VADDR);
    assert(plan.virtual_end == TEST_DATA_VADDR + 16u);
    assert(plan.image_span == 0xf10);
    assert(plan.segment_count == 2);

    assert(elf64_load_contiguous(image, sizeof(image),
                                 destination, sizeof(destination),
                                 &plan) == ELF64_LOAD_OK);
    assert(destination[0] == 0x31);
    assert(destination[1] == 0xc0);
    assert(destination[2] == 0xc3);
    assert(destination[3] == 0x90);
    assert(destination[4] == 0); /* code BSS */
    assert(destination[0xeff] == 0);
    assert(destination[0xf00] == 'A');
    assert(destination[0xf01] == 'P');
    assert(destination[0xf02] == 'P');
    assert(destination[0xf03] == 0); /* data BSS */
}

static void test_elf_rejections(void) {
    uint8_t image[TEST_IMAGE_SIZE];
    uint8_t destination[0x1000];
    struct elf64_image_plan plan;

    make_valid_elf(image);
    image[0] = 0;
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_BAD_MAGIC);

    make_valid_elf(image);
    put_u16(image + 18, 183); /* EM_AARCH64 */
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_UNSUPPORTED_ARCHITECTURE);

    make_valid_elf(image);
    put_u64(image + 24, 0x400100);
    put_u64(program_header(image, 0) + 16, 0x400100);
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_SEGMENT_OUT_OF_RANGE);

    make_valid_elf(image);
    put_u64(program_header(image, 1) + 16,
            ELF64_LOADER_MAX_USER_VADDR);
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_SEGMENT_OUT_OF_RANGE);

    make_valid_elf(image);
    assert(elf64_inspect(image, 80, &plan) == ELF64_LOAD_TRUNCATED);

    make_valid_elf(image);
    put_u32(program_header(image, 1), 2); /* PT_DYNAMIC */
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_UNSUPPORTED_FEATURE);

    make_valid_elf(image);
    put_u32(program_header(image, 0) + 4,
            ELF64_SEGMENT_READ |
            ELF64_SEGMENT_WRITE |
            ELF64_SEGMENT_EXECUTE);
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_WRITABLE_CODE);

    make_valid_elf(image);
    put_u64(program_header(image, 1) + 16,
            TEST_CODE_VADDR + 0x100u);
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_WRITABLE_CODE);

    make_valid_elf(image);
    put_u64(program_header(image, 1) + 16, TEST_CODE_VADDR + 4u);
    put_u64(program_header(image, 1) + 48, 1);
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_SEGMENT_OVERLAP);

    make_valid_elf(image);
    put_u64(image + 24,
            TEST_CODE_VADDR + 6u); /* executable BSS, not file bytes */
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_INVALID_ENTRY_POINT);

    make_valid_elf(image);
    put_u64(program_header(image, 1) + 16,
            TEST_CODE_VADDR + 0x1100f00ull);
    assert(elf64_inspect(image, sizeof(image), &plan) ==
           ELF64_LOAD_IMAGE_TOO_LARGE);

    make_valid_elf(image);
    assert(elf64_load_contiguous(image, sizeof(image),
                                 destination, 16, NULL) ==
           ELF64_LOAD_DESTINATION_TOO_SMALL);

    make_valid_elf(image);
    assert(elf64_load_contiguous(image, sizeof(image),
                                 image + 0x80, 0x1000, NULL) ==
           ELF64_LOAD_BUFFER_OVERLAP);
}

static void test_manifest_registry(void) {
    struct app_manifest manifest = valid_manifest();
    app_registry_reset();

    assert(app_manifest_validate(&manifest) == APP_MANIFEST_OK);
    assert(app_registry_register(&manifest) == APP_MANIFEST_OK);
    assert(app_registry_count() == 1);
    assert(app_registry_find_id("test-app") != NULL);
    assert(app_registry_find_executable("test-app.elf") != NULL);
    assert(app_registry_at(1) == NULL);

    assert(app_registry_register(&manifest) ==
           APP_MANIFEST_DUPLICATE_ID);

    struct app_manifest second = manifest;
    memcpy(second.id, "second", sizeof("second"));
    assert(app_registry_register(&second) ==
           APP_MANIFEST_DUPLICATE_EXECUTABLE);

    manifest.capabilities = 1ull << 63;
    assert(app_manifest_validate(&manifest) ==
           APP_MANIFEST_INVALID_CAPABILITY);

    manifest = valid_manifest();
    memcpy(manifest.id, "Bad Id", sizeof("Bad Id"));
    assert(app_manifest_validate(&manifest) ==
           APP_MANIFEST_INVALID_ID);

    manifest = valid_manifest();
    memcpy(manifest.executable, "../bad.elf", sizeof("../bad.elf"));
    assert(app_manifest_validate(&manifest) ==
           APP_MANIFEST_INVALID_EXECUTABLE);
}

static void test_abi_contract(void) {
    assert(app_abi_syscall_known(APP_SYSCALL_ABI_QUERY));
    assert(app_abi_syscall_known(APP_SYSCALL_MEMORY_UNMAP));
    assert(!app_abi_syscall_known(0));
    assert(app_abi_required_capability(APP_SYSCALL_TIME_GET) ==
           APP_CAPABILITY_TIME);
    assert(app_abi_required_capability(APP_SYSCALL_FILE_READ) ==
           APP_CAPABILITY_FILE_READ);
    assert(app_abi_required_capability(UINT64_MAX) == UINT64_MAX);
    assert(strcmp(app_abi_syscall_name(APP_SYSCALL_WINDOW_CREATE),
                  "window_create") == 0);
}

static void test_process_metadata(void) {
    uint8_t image[TEST_IMAGE_SIZE];
    struct elf64_image_plan plan;
    struct app_manifest manifest = valid_manifest();
    struct app_process_info process;
    uint64_t process_id = 0;
    make_valid_elf(image);
    assert(elf64_inspect(image, sizeof(image), &plan) == ELF64_LOAD_OK);

    app_process_table_reset();
    uint64_t cancelled_process_id = 0;
    assert(app_process_track_loaded(&manifest, &plan,
                                    &cancelled_process_id) ==
           APP_PROCESS_OK);
    assert(app_process_release(cancelled_process_id) == APP_PROCESS_OK);
    assert(app_process_count() == 0);

    assert(app_process_track_loaded(&manifest, &plan, &process_id) ==
           APP_PROCESS_OK);
    assert(process_id != 0);
    assert(app_process_count() == 1);
    assert(app_process_find(process_id, &process));
    assert(process.state == APP_PROCESS_LOADED);
    assert(strcmp(process.app_id, "test-app") == 0);
    assert(process.entry_point == plan.entry_point);

    assert(app_process_mark_running(process_id) ==
           APP_PROCESS_INVALID_TRANSITION);
    assert(app_process_mark_starting(process_id) == APP_PROCESS_OK);
    assert(app_process_mark_running(process_id) == APP_PROCESS_OK);

    const struct app_fault_record fault = {
        .vector = 14,
        .has_error_code = true,
        .error_code = 5,
        .instruction_pointer = plan.entry_point,
        .stack_pointer = ELF64_LOADER_MAX_USER_VADDR - 0x1000u,
        .fault_address = 0xdeadbeef,
    };
    assert(app_process_record_fault(process_id, &fault) ==
           APP_PROCESS_OK);
    assert(app_process_find(process_id, &process));
    assert(process.state == APP_PROCESS_FAULTED);
    assert(process.fault.vector == 14);
    assert(process.fault.fault_address == 0xdeadbeef);
    assert(app_process_release(process_id) == APP_PROCESS_OK);
    assert(app_process_count() == 0);
    assert(!app_process_find(process_id, &process));

    uint64_t history_ids[APP_PROCESS_CAPACITY + 1u];
    for (size_t index = 0;
         index < APP_PROCESS_CAPACITY + 1u; index++) {
        assert(app_process_track_loaded(&manifest, &plan,
                                        &history_ids[index]) ==
               APP_PROCESS_OK);
        assert(app_process_mark_starting(history_ids[index]) ==
               APP_PROCESS_OK);
        assert(app_process_mark_exited(history_ids[index],
                                       (int64_t)index) ==
               APP_PROCESS_OK);
    }
    assert(app_process_count() == APP_PROCESS_CAPACITY);
    assert(!app_process_find(history_ids[0], &process));
    assert(app_process_find(history_ids[APP_PROCESS_CAPACITY], &process));
    for (size_t index = 0; index < APP_PROCESS_CAPACITY; index++) {
        assert(app_process_snapshot(index, &process));
        assert(process.process_id == history_ids[index + 1u]);
    }

    assert((app_runtime_features() & APP_RUNTIME_ELF_LOADING) != 0);
    assert((app_runtime_features() & APP_RUNTIME_USER_EXECUTION) != 0);
    assert((app_runtime_features() & APP_RUNTIME_ADDRESS_ISOLATION) != 0);
    assert((app_runtime_features() & APP_RUNTIME_FAULT_RECOVERY) != 0);
    assert(app_runtime_execution_implemented());
}

static void test_toolchain_elf(const char* path) {
    FILE* file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long file_length = ftell(file);
    assert(file_length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);

    uint8_t* image = (uint8_t*)malloc((size_t)file_length);
    assert(image != NULL);
    assert(fread(image, 1, (size_t)file_length, file) ==
           (size_t)file_length);
    assert(fclose(file) == 0);

    struct elf64_image_plan plan;
    assert(elf64_inspect(image, (size_t)file_length, &plan) ==
           ELF64_LOAD_OK);
    uint8_t* loaded = (uint8_t*)malloc(plan.image_span);
    assert(loaded != NULL);
    assert(elf64_load_contiguous(image, (size_t)file_length,
                                 loaded, plan.image_span, NULL) ==
           ELF64_LOAD_OK);
    assert(plan.entry_point >= plan.virtual_base);
    assert(plan.entry_point < plan.virtual_end);

    app_catalog_reset();
    size_t catalog_index = SIZE_MAX;
    assert(app_catalog_install_hello(image, (size_t)file_length,
                                     &catalog_index) == APP_CATALOG_OK);
    assert(catalog_index == 0);
    assert(app_catalog_count() == 1);
    const struct app_catalog_entry* entry =
        app_catalog_find_id("hello");
    assert(entry != NULL);
    assert(strcmp(entry->manifest.executable, "hello.elf") == 0);
    assert(entry->manifest.capabilities ==
           (APP_CAPABILITY_LOG | APP_CAPABILITY_TIME));
    assert(entry->image == image);
    assert(entry->image_size == (size_t)file_length);
    assert(entry->image_plan.entry_point == plan.entry_point);
    assert(app_catalog_install_hello(image, (size_t)file_length, NULL) ==
           APP_CATALOG_REGISTRY_REJECTED);
    app_catalog_reset();

    free(loaded);
    free(image);
}

static void test_fault_probe_elf(const char* path) {
    FILE* file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long file_length = ftell(file);
    assert(file_length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);

    uint8_t* image = (uint8_t*)malloc((size_t)file_length);
    assert(image != NULL);
    assert(fread(image, 1, (size_t)file_length, file) ==
           (size_t)file_length);
    assert(fclose(file) == 0);

    struct elf64_image_plan plan;
    assert(elf64_inspect(image, (size_t)file_length, &plan) ==
           ELF64_LOAD_OK);
    uint8_t* loaded = (uint8_t*)malloc(plan.image_span);
    assert(loaded != NULL);
    assert(elf64_load_contiguous(image, (size_t)file_length,
                                 loaded, plan.image_span, NULL) ==
           ELF64_LOAD_OK);
    assert(plan.entry_point >= plan.virtual_base);
    assert(plan.entry_point < plan.virtual_end);

    app_catalog_reset();
    assert(app_catalog_install_fault_probe(
               image, (size_t)file_length, NULL) == APP_CATALOG_OK);
    const struct app_catalog_entry* entry =
        app_catalog_find_id("fault-probe");
    assert(entry != NULL);
    assert(strcmp(entry->manifest.executable,
                  "fault-probe.elf") == 0);
    assert(entry->manifest.capabilities == 0);
    app_catalog_reset();

    free(loaded);
    free(image);
}

static void test_hang_probe_elf(const char* path) {
    FILE* file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long file_length = ftell(file);
    assert(file_length > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);

    uint8_t* image = (uint8_t*)malloc((size_t)file_length);
    assert(image != NULL);
    assert(fread(image, 1, (size_t)file_length, file) ==
           (size_t)file_length);
    assert(fclose(file) == 0);

    struct elf64_image_plan plan;
    assert(elf64_inspect(image, (size_t)file_length, &plan) ==
           ELF64_LOAD_OK);

    app_catalog_reset();
    assert(app_catalog_install_hang_probe(
               image, (size_t)file_length, NULL) == APP_CATALOG_OK);
    const struct app_catalog_entry* entry =
        app_catalog_find_id("hang-probe");
    assert(entry != NULL);
    assert(strcmp(entry->manifest.executable,
                  "hang-probe.elf") == 0);
    assert(entry->manifest.capabilities == 0);
    assert(entry->image_plan.entry_point == plan.entry_point);
    app_catalog_reset();

    free(image);
}

int main(int argc, char** argv) {
    test_valid_inspect_and_load();
    test_elf_rejections();
    test_manifest_registry();
    test_abi_contract();
    test_process_metadata();
    if (argc == 4) {
        test_toolchain_elf(argv[1]);
        test_fault_probe_elf(argv[2]);
        test_hang_probe_elf(argv[3]);
    } else {
        assert(argc == 1);
    }
    puts("apps_v1_test: all tests passed");
    return 0;
}
