BITS 64
DEFAULT ABS

section .rodata.apps align=16
    global nostalux_image_viewer_app_start
    global nostalux_image_viewer_app_end

nostalux_image_viewer_app_start:
    incbin "build/apps/image-viewer.elf"
nostalux_image_viewer_app_end:

section .note.GNU-stack noalloc noexec nowrite progbits
