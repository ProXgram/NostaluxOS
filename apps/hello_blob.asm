BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_hello_app_start
    global nostalux_hello_app_end

nostalux_hello_app_start:
    incbin "build/apps/hello.elf"
nostalux_hello_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
