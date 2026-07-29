BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_notepad_app_start
    global nostalux_notepad_app_end

nostalux_notepad_app_start:
    incbin "build/apps/notepad.elf"
nostalux_notepad_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
