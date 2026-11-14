; Millicode for 68040 exception processing: the exception entry point, the
; instructions that raise exceptions (TRAP, TRAPV, TRAPcc, ILLEGAL, line A),
; and RTE.
;
; Exception entry (spec/uisa.yaml, entry point `exception`), reached by
; gfault / gfaultr or a decode-time condition:
;   x1 = vector number
;   x2 = PC to stack (gpc when the exception was raised: the instruction's own
;        address for faults, the next instruction for traps)
;   x3 = address of the instruction that raised it (the format 2 address)
;   x4 = fault status (access errors; not used yet)
; The call stack is empty on entry.

exception:
        rdccr   x10
        or      x10, x10, gsr           ; SR to stack
        andi    x5, gsr, 0x1000         ; M = 1 (master stack): not handled yet
        b.eq    x5, x0, exc_m0
        brk     7
exc_m0:
        andi    x5, gsr, 0x2000
        b.ne    x5, x0, exc_super
        csrw    usp, a7                 ; user -> supervisor stack
        csrr    a7, isp
exc_super:
        andi    x5, gsr, 0x0700         ; keep the interrupt mask; T1 = T0 = 0
        ori     gsr, x5, 0x2000         ; S = 1
        csrr    x12, vbr
        sh2add  x12, x12, x1
        ld.32.gd x13, 0(x12)            ; handler address
        slli    x14, x1, 2              ; vector offset
        addi    x5, x0, 5               ; zero divide, CHK, TRAPV/TRAPcc, trace:
        b.eq    x1, x5, exc_fmt2        ;   six-word frame with the instruction address
        addi    x5, x0, 6
        b.eq    x1, x5, exc_fmt2
        addi    x5, x0, 7
        b.eq    x1, x5, exc_fmt2
        addi    x5, x0, 9
        b.eq    x1, x5, exc_fmt2
        addi    a7, a7, -8              ; format 0: SR, PC, format/vector
        jal     x0, exc_frame
exc_fmt2:
        addi    a7, a7, -12
        st.32.gd x3, 8(a7)
        ori     x14, x14, 0x2000        ; format 2
exc_frame:
        st.16.gd x10, 0(a7)
        st.32.gd x2, 2(a7)
        st.16.gd x14, 6(a7)
        add     gpc, x13, x0
        gexit

; ---------------------------------------------------------------------------
; TRAP #n, TRAPV, TRAPcc, ILLEGAL, line A
; ---------------------------------------------------------------------------
trap:
        andi    x5, x1, 15
        addi    x5, x5, 32
        add     gpc, x4, x0             ; traps stack the next instruction
        gfaultr x5

trapv:
        rdccr   x5
        andi    x5, x5, 2               ; V
        b.eq    x5, x0, exit
        add     gpc, x4, x0
        gfault  7

trapcc:
        srli    x5, x1, 8
        andi    x5, x5, 15
        call    cond_eval
        b.eq    x10, x0, exit
        add     gpc, x4, x0
        gfault  7

illegal_insn:
        gfault  4

line_a:
        gfault  10

; ---------------------------------------------------------------------------
; RTE: formats 0, 2, 3, 4 and 7 (access error; restarts the instruction).
; Format 1 (throwaway) frames and frames that continue a trace are not
; handled yet.
; ---------------------------------------------------------------------------
rte:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        ld.16u.gd x10, 0(a7)            ; SR
        ld.32.gd x20, 2(a7)             ; PC (set_sr uses x11-x15)
        ld.16u.gd x12, 6(a7)            ; format / vector offset
        srli    x13, x12, 12
        b.eq    x13, x0, rte_8
        addi    x6, x0, 2
        b.eq    x13, x6, rte_12
        addi    x6, x0, 3
        b.eq    x13, x6, rte_12
        addi    x6, x0, 4
        b.eq    x13, x6, rte_16
        addi    x6, x0, 7
        b.eq    x13, x6, rte_fmt7
        addi    x6, x0, 1
        b.eq    x13, x6, rte_fmt1
        gfault  14                      ; format error
rte_fmt1:
        brk     8                       ; throwaway frame: not handled yet
rte_8:
        addi    a7, a7, 8
        jal     x0, rte_done
rte_12:
        addi    a7, a7, 12
        jal     x0, rte_done
rte_16:
        addi    a7, a7, 16
        jal     x0, rte_done
rte_fmt7:
        ld.16u.gd x14, 12(a7)           ; SSW
        andi    x14, x14, 0x2000        ; CT (continue a trace): not handled yet
        b.eq    x14, x0, rte_fmt7_pop
        brk     9
rte_fmt7_pop:                           ; CM (MOVEM continuation) restarts the MOVEM
        addi    a7, a7, 60
rte_done:
        call    set_sr                  ; after the pop: may switch to the user stack
        add     gpc, x20, x0
        gexit
