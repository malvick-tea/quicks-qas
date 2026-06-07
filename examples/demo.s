; qas demo input (Intel syntax). Not yet assembled — used to exercise the lexer
; via `qas --dump-tokens examples/demo.s`.

.text
start:
    mov rax, 0x3C          ; syscall number (example)
    mov rdi, 0
    lea rbx, [rbp + rcx*8 - 16]
    add rax, 0b1010
    ret
