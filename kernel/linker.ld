ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text : ALIGN(0x1000)
    {
        __text_start = .;
        *(.text*)
        __text_end = .;
    }

    . = ALIGN(0x1000);
    .rodata :
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    . = ALIGN(0x1000);
    .data :
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    . = ALIGN(0x1000);
    .bss :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;
}

ASSERT(__kernel_end <= 0x001F0000,
       "Kernel image exceeds safe limit and would collide with the long mode stack");
