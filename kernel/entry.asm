BITS 64

section .text
    global _start
    global context_switch
    global task_return_trampoline
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

    sti

    mov rdi, r12
    call kmain

.hang:
    hlt
    jmp .hang

; void context_switch(uint64_t* old_sp_ptr, uint64_t new_sp)
context_switch:
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

    ; The ring-3 interrupt frame plus the fourteen pushes above leave RSP at
    ; 8 mod 16. Align the caller stack as required by the SysV x86-64 ABI
    ; without changing the register-frame pointer passed in RDI.
    sub rsp, 8
    
    ; 3. Call Kernel Dispatcher
    ; uint64_t syscall_dispatcher(struct syscall_regs* regs)
    call syscall_dispatcher
    add rsp, 8
    
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
