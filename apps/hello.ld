OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(nostalux_app_entry)

PHDRS
{
    /*
     * Include the ELF/program headers in the read-execute image.  This keeps
     * a small static application compact on disk instead of spending the
     * first 4 KiB on padding before its only PT_LOAD segment.
     */
    image PT_LOAD FILEHDR PHDRS FLAGS(5); /* Read + execute, never writable. */
    data PT_LOAD FLAGS(6);       /* Read + write, never executable. */
    stack PT_GNU_STACK FLAGS(6);  /* Read + write, never executable. */
}

SECTIONS
{
    /*
     * Apps live in the reserved 1-2 TiB runtime window. Keeping them out of
     * PML4[0] prevents a user mapping from replacing the kernel/framebuffer
     * identity map.
     */
    . = 0x0000010000400000 + SIZEOF_HEADERS;

    .text : ALIGN(16)
    {
        *(.text .text.*)
    } :image

    .rodata : ALIGN(16)
    {
        *(.rodata .rodata.*)
    } :image

    /*
     * x86 permissions apply to complete pages. Start mutable state on the
     * next page so it can never make a code page writable. Apps without
     * mutable state do not get a data PT_LOAD from GNU ld.
     */
    . = ALIGN(0x1000);

    .data : ALIGN(16)
    {
        *(.data .data.*)
    } :data

    .bss (NOLOAD) : ALIGN(16)
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.comment)
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }

    __app_image_end = .;
}

ASSERT(SIZEOF(.text) > 0, "App has no executable code")
ASSERT(__app_image_end <= 0x0000010000410000,
       "App exceeds its 64 KiB virtual image budget")
