%ifndef KERNEL_SIZE_BYTES
%error "KERNEL_SIZE_BYTES must be defined at assembly time"
%endif

BITS 16
ORG 0x7E00

CODE32_SEG     equ 0x08
DATA_SEG       equ 0x10
CODE64_SEG     equ 0x18
KERNEL_DEST    equ 0x00100000

%ifndef PROTECTED_STACK
PROTECTED_STACK equ 0x00280000
%endif

%ifndef LONG_STACK_TOP
LONG_STACK_TOP  equ 0x003FF000
%endif

; Paging Structures
PML4            equ 0x00200000
PDPT            equ 0x00201000
PD              equ 0x00202000

BOOT_INFO       equ 0x00005000
E820_BUFFER     equ 0x00007000
E820_MAX_ENTRIES equ 64

; VESA VBE Structures
VBE_INFO_ADDR   equ 0x00006000
MODE_INFO_ADDR  equ 0x00006200

stage2_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7E00

    ; Ask the BIOS for a real physical memory map before entering protected
    ; mode. This avoids probing arbitrary addresses and potentially touching
    ; MMIO or firmware-reserved holes later in the kernel.
    call collect_e820

    ; --- VESA VBE SETUP ---
    mov di, VBE_INFO_ADDR
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    mov fs, word [VBE_INFO_ADDR + 16]
    mov si, word [VBE_INFO_ADDR + 14]

.find_mode:
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je .vbe_fail
    add si, 2

    mov ax, 0x4F01
    mov di, MODE_INFO_ADDR
    int 0x10
    cmp ax, 0x004F
    jne .find_mode

    ; Look for 800x600x32 (Changed from 1024x768 to make things appear larger)
    mov ax, [MODE_INFO_ADDR + 18]
    cmp ax, 800
    jne .find_mode
    mov ax, [MODE_INFO_ADDR + 20]
    cmp ax, 600
    jne .find_mode
    mov al, [MODE_INFO_ADDR + 25]
    cmp al, 32
    jne .find_mode

    mov ax, [MODE_INFO_ADDR + 0]
    and ax, 0x0080
    jz .find_mode

    mov bx, cx
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .vbe_fail

    ; Fill BootInfo
    mov ax, [MODE_INFO_ADDR + 18]
    mov dword [BOOT_INFO + 0], 0
    mov word [BOOT_INFO + 0], ax
    mov ax, [MODE_INFO_ADDR + 20]
    mov dword [BOOT_INFO + 4], 0
    mov word [BOOT_INFO + 4], ax
    mov ax, [MODE_INFO_ADDR + 16]
    mov dword [BOOT_INFO + 8], 0
    mov word [BOOT_INFO + 8], ax
    xor eax, eax
    mov al, [MODE_INFO_ADDR + 25]
    mov dword [BOOT_INFO + 12], eax
    mov eax, [MODE_INFO_ADDR + 40]
    mov dword [BOOT_INFO + 16], eax
    mov dword [BOOT_INFO + 20], 0
    xor eax, eax
    mov ax, [e820_count]
    mov dword [BOOT_INFO + 24], eax
    mov dword [BOOT_INFO + 28], 24
    mov dword [BOOT_INFO + 32], E820_BUFFER
    mov dword [BOOT_INFO + 36], 0

    jmp .enable_pm

.vbe_fail:
    cli
    hlt
    jmp .vbe_fail

.enable_pm:
    call enable_a20
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp CODE32_SEG:protected_mode_entry

enable_a20:
    in al, 0x92
    or al, 0x02
    out 0x92, al
    ret

collect_e820:
    xor ebx, ebx
    xor bp, bp
    mov di, E820_BUFFER

.next:
    xor ax, ax
    mov es, ax
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [es:di + 20], 1
    push bp
    push di
    int 0x15
    pop di
    pop bp
    jc .done
    cmp eax, 0x534D4150
    jne .failed
    cmp ecx, 20
    jb .failed

    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .continue
    cmp ecx, 20
    jbe .keep
    test dword [es:di + 20], 1
    jz .continue

.keep:
    inc bp
    add di, 24
    cmp bp, E820_MAX_ENTRIES
    jae .done

.continue:
    test ebx, ebx
    jnz .next

.done:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov [e820_count], bp
    ret

.failed:
    xor bp, bp
    jmp .done

e820_count:
    dw 0

[BITS 32]
protected_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov esp, PROTECTED_STACK

    cld
    mov esi, stage2_end
    mov edi, KERNEL_DEST
    mov ecx, KERNEL_SIZE_BYTES
    rep movsb

    ; --- Paging Setup (Identity Map First 1GB) ---
    ; Clear tables
    mov edi, PML4
    xor eax, eax
    mov ecx, 4096
    rep stosd 

    ; PML4[0] -> PDPT
    mov eax, PDPT | 0x3
    mov [PML4], eax

    ; PDPT[0] -> PD
    mov eax, PD | 0x3
    mov [PDPT], eax

    ; Map 0-1GB using 2MB Pages in PD
    mov ecx, 512
    xor eax, eax
    mov edi, PD
.map_low:
    mov edx, eax
    or edx, 0x83 ; Present | RW | HugePage (2MB)
    mov [edi], edx
    add eax, 0x200000
    add edi, 8
    loop .map_low

    ; Enable Long Mode
    mov eax, PML4
    mov cr3, eax
    mov eax, cr4
    or eax, (1 << 5) ; PAE
    mov cr4, eax
    
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8) ; LME
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) ; PG
    mov cr0, eax

    jmp CODE64_SEG:long_mode_entry

[BITS 64]
long_mode_entry:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, LONG_STACK_TOP
    
    mov rdi, BOOT_INFO
    mov rax, KERNEL_DEST
    call rax

.hang:
    hlt
    jmp .hang

[BITS 16]
gdt_start:
    dq 0
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
    dw 0x0000, 0x0000, 0x9A00, 0x0020
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

stage2_end:
