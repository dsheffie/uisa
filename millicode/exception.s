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
;   Access faults (vector 2) take the format 7 frame fields from the fault_*
;   and wb* CSRs.
; The call stack is empty on entry.
; The frame's longwords at word (not longword) offsets -- the PC at 2, the
; format 2 address at 8 -- are stored and loaded as two words: a 68k stack
; pointer is only word-aligned in general (an exception frame alone moves it
; by 8 or 12), and an implementation may require natural alignment for a
; longword access (superdupersparc's core does).

exception:
        call    exc_enter
        andi    x5, gsr, 0x0700         ; keep the interrupt mask; T1 = T0 = 0
        ori     gsr, x5, 0x2000         ; S = 1
        call    exc_vector
        addi    x5, x0, 2
        b.eq    x1, x5, exc_fmt7        ; access fault
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
        srli    x5, x3, 16              ; the instruction address, as two words
        st.16.gd x5, 8(a7)
        st.16.gd x3, 10(a7)
        ori     x14, x14, 0x2000        ; format 2
        jal     x0, exc_frame
; Format 7 (access error), 30 words, from the fault CSRs.
exc_fmt7:
        addi    a7, a7, -60
        csrr    x5, fault_ea
        st.32.gd x5, 8(a7)              ; EA
        csrr    x5, fault_status
        st.16.gd x5, 12(a7)             ; SSW
        csrr    x5, wb3_status
        st.16.gd x5, 14(a7)             ; WB3S
        csrr    x5, wb2_status
        st.16.gd x5, 16(a7)             ; WB2S
        st.16.gd x0, 18(a7)             ; WB1S
        csrr    x5, fault_addr
        st.32.gd x5, 20(a7)             ; FA
        st.32.gd x5, 24(a7)             ; WB3A
        csrr    x5, wb3_data
        st.32.gd x5, 28(a7)             ; WB3D
        csrr    x5, wb2_addr
        st.32.gd x5, 32(a7)             ; WB2A
        st.32.gd x0, 36(a7)             ; WB2D
        st.32.gd x0, 40(a7)             ; WB1A
        csrr    x5, fault_pd0
        st.32.gd x5, 44(a7)             ; WB1D / PD0
        csrr    x5, fault_pd1
        st.32.gd x5, 48(a7)
        csrr    x5, fault_pd2
        st.32.gd x5, 52(a7)
        csrr    x5, fault_pd3
        st.32.gd x5, 56(a7)
        addi    x5, x0, 7               ; format 7
        slli    x5, x5, 12
        or      x14, x14, x5
exc_frame:
        st.16.gd x10, 0(a7)
        srli    x5, x2, 16              ; the PC, as two words
        st.16.gd x5, 2(a7)
        st.16.gd x2, 4(a7)
        st.16.gd x14, 6(a7)
        add     gpc, x13, x0
        gexit

; Interrupt entry (spec entry point `interrupt`), taken at an instruction
; boundary: x1 = level, x2 = vector. Format 0 frame with the next PC.
interrupt:
        call    exc_enter
        slli    x5, x1, 8
        ori     gsr, x5, 0x2000         ; S = 1, T = 0, mask = level
        add     x1, x2, x0
        add     x2, gpc, x0
        call    exc_vector
        addi    a7, a7, -8
        jal     x0, exc_frame

; x10 = SR to stack; switch to the supervisor stack.
exc_enter:
        rdccr   x10
        or      x10, x10, gsr
        andi    x5, gsr, 0x1000         ; M = 1 (master stack): not handled yet
        b.eq    x5, x0, exc_m0
        brk     7
exc_m0:
        andi    x5, gsr, 0x2000
        b.ne    x5, x0, exc_enter_done
        csrw    usp, a7                 ; user -> supervisor stack
        csrr    a7, isp
exc_enter_done:
        ret

; x13 = handler address for vector x1; x14 = vector offset
exc_vector:
        csrr    x12, vbr
        sh2add  x12, x12, x1
        ld.32.gd x13, 0(x12)
        slli    x14, x1, 2
        ret

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
        ld.16u.gd x20, 2(a7)            ; PC, as two words (set_sr uses x11-x15)
        ld.16u.gd x5, 4(a7)
        slli    x20, x20, 16
        or      x20, x20, x5
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
