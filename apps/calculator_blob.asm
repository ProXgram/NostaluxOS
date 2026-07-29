BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_calculator_app_start
    global nostalux_calculator_app_end

nostalux_calculator_app_start:
    incbin "build/apps/calculator.elf"
nostalux_calculator_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
