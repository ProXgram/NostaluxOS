BITS 64
DEFAULT ABS

section .text
    global _start
    global context_switch
    global task_return_trampoline
    global enter_user_mode
    global isr_syscall
    
    extern kmain
    extern gdt_init
    extern interrupts_init
    extern paging_init
    extern syslog_init
    extern __bss_start
    extern __bss_end
    extern g_kernel_stack_top
    extern syscall_dispatcher
    extern exit_current_task

_start:
    cli
    cld
    mov rbp, 0

    mov rsp, [g_kernel_stack_top]
    and rsp, -16

    mov r12, rdi ; BootInfo

    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor rax, rax
    rep stosb

    call syslog_init
    
    mov rdi, r12
    call paging_init
    
    call gdt_init
    call interrupts_init

    mov rdi, r12
    call kmain

.hang:
    hlt
    jmp .hang

; void context_switch(uint64_t* old_sp_ptr, uint64_t new_sp,
;                     uint64_t saved_rflags)
context_switch:
    ; schedule() captured RFLAGS before entering its CLI-protected CR3/task
    ; transition. Store that exact value in this task's context.
    push rdx
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; Save old RSP
    mov rsp, rsi        ; Load new RSP

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    popfq
    ret

; A task entry is reached via RET with a synthetic return address on its stack.
; If the entry function returns, RSP is already correctly aligned for a new
; CALL here (0 mod 16).
task_return_trampoline:
    call exit_current_task
.task_hang:
    cli
    hlt
    jmp .task_hang

; _Noreturn void enter_user_mode(uint64_t entry_point, uint64_t user_stack_top)
enter_user_mode:
    cli
    mov ax, 0x1b             ; User data selector (GDT 0x18 | RPL3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; IRET enters the ELF entry point directly rather than through CALL.
    ; Reserve a synthetic return slot so the SysV function-entry invariant is
    ; RSP % 16 == 8. Returning unexpectedly faults at address zero and remains
    ; contained as an ordinary ring-3 process fault.
    sub rsi, 8
    mov qword [rsi], 0

    push qword 0x1b          ; SS
    push rsi                 ; RSP
    push qword 0x202         ; RFLAGS: reserved bit + IF
    push qword 0x23          ; CS (GDT 0x20 | RPL3)
    push rdi                 ; RIP

    ; The C bootstrap may retain kernel pointers and other privileged data in
    ; caller- or callee-saved registers. Start every process with a clean,
    ; deterministic general-register file.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; ---------------------------------------------
; System Call Entry Point (INT 0x80)
; ---------------------------------------------
isr_syscall:
    ; User code may set DF. The SysV ABI requires it clear before entering C.
    cld

    ; 1. Save User State
    push rbp
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    
    ; 2. Pass stack pointer (regs) to C function
    mov rdi, rsp 

    ; Preserve the register-frame address in a callee-saved register and align
    ; dynamically. Ring 3 normally arrives at 8 mod 16 here, while an
    ; accidental CPL0 INT 0x80 has a shorter hardware frame; both paths now
    ; satisfy the SysV x86-64 call boundary.
    mov r12, rsp
    and rsp, -16
    
    ; 3. Call Kernel Dispatcher
    ; uint64_t syscall_dispatcher(struct syscall_regs* regs)
    call syscall_dispatcher
    mov rsp, r12
    
    ; 4. Restore User State
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    pop rbp
    
    ; RAX contains return value from syscall_dispatcher
    
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
