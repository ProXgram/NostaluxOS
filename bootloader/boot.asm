
BITS 16
ORG 0x7C00

%ifndef TOTAL_SECTORS
%error "TOTAL_SECTORS must be defined at assembly time"
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [boot_drive], dl

    mov si, disk_address_packet
    mov word [si + 4], stage2_offset & 0x000F
    mov word [si + 6], stage2_offset >> 4
    mov dword [si + 8], 1
    mov dword [si + 12], 0

    mov dword [buffer_pointer], stage2_offset

    mov cx, TOTAL_SECTORS

    ; INT 13h extensions only guarantee up to 127 sectors per transfer.
    ; Load the payload in manageable chunks so larger kernels boot reliably.

.load_loop:
    cmp cx, 0
    je .load_done

    mov ax, cx
    cmp ax, 127
    jbe .chunk_ready
    mov ax, 127
.chunk_ready:
    mov si, disk_address_packet
    mov word [si + 2], ax
    push cx
    push ax

    mov dl, [boot_drive]
    mov ah, 0x42
    mov si, disk_address_packet
    int 0x13
    jc disk_error

    pop ax
    pop cx
    sub cx, ax

    ; Advance the disk LBA by the number of sectors just read. Do this before
    ; AX is reused to calculate the next destination segment.
    mov si, disk_address_packet
    add word [si + 8], ax
    adc word [si + 10], 0
    adc word [si + 12], 0
    adc word [si + 14], 0

    movzx eax, ax
    shl eax, 9
    add dword [buffer_pointer], eax

    mov si, disk_address_packet
    mov eax, [buffer_pointer]
    mov dx, ax
    and dx, 0x000F
    mov [si + 4], dx
    shr eax, 4
    mov [si + 6], ax

    jmp .load_loop

.load_done:
    jmp 0x0000:stage2_offset

disk_error:
    mov si, disk_error_message
.print_loop:
    lodsb
    or al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print_loop
.halt:
    cli
    hlt
    jmp .halt

boot_drive: db 0
stage2_offset equ 0x7E00
buffer_pointer: dd stage2_offset

disk_address_packet:
    db 0x10
    db 0x00
    dw 0x0000
    dw stage2_offset
    dw 0x0000
    dq 1

disk_error_message db 'Disk read error!', 0

TIMES 510 - ($ - $$) db 0
dw 0xAA55
