BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_hang_probe_app_start
    global nostalux_hang_probe_app_end

nostalux_hang_probe_app_start:
    incbin "build/apps/hang-probe.elf"
nostalux_hang_probe_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
