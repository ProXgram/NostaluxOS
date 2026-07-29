BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_ai_assistant_app_start
    global nostalux_ai_assistant_app_end

nostalux_ai_assistant_app_start:
    incbin "build/apps/ai-assistant.elf"
nostalux_ai_assistant_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
