BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_rflags_probe_app_start
    global nostalux_rflags_probe_app_end

nostalux_rflags_probe_app_start:
    incbin "build/apps/rflags-probe.elf"
nostalux_rflags_probe_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
