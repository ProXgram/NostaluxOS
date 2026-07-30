BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_browser_app_start
    global nostalux_browser_app_end

nostalux_browser_app_start:
    incbin "build/apps/browser.elf"
nostalux_browser_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
