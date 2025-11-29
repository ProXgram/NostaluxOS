; This is a comment. The computer ignores lines starting with ;

[BITS 16]           ; Tell the computer we are in 16-bit mode (Old school mode!)
[ORG 0x7C00]        ; Tell the computer: "We are sitting at address 0x7C00"

start:
    ; This is where the code begins!
    
    jmp $           ; "Jump to here". This is an infinite loop. 
                    ; The computer will spin here forever doing nothing.
                    ; This prevents it from crashing.

; PADDING ------------------------------------------------------------------
; We need the file to be EXACTLY 512 bytes.
; We have written a few bytes of code above.
; Now we fill the rest of the space with zeros until we reach byte 510.
times 510-($-$$) db 0 

; THE MAGIC SIGNATURE ------------------------------------------------------
; This occupies bytes 511 and 512.
; This tells the BIOS: "Yes, I am a bootable disk!"
dw 0xAA55
