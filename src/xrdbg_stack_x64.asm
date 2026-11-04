; Enter a fully committed parking stack. RCX=worker, RDX=top, R8=entry.
; The C entry restores the captured CONTEXT and never returns. No old frames
; are active on this stack; no unwind across this boundary is supported.
PUBLIC xrdbg_native_stack_enter
.code
xrdbg_native_stack_enter PROC
    mov rsp, rdx
    and rsp, -16
    sub rsp, 32
    call r8
    int 3
xrdbg_native_stack_enter ENDP
END

