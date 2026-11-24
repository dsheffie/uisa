; Millicode for DIVU.W / DIVS.W and DIVU.L / DIVS.L.
;
; 68040 behaviour (as Previous models it): on overflow V = 1, C = 0 and the
; destination registers, N and Z are unchanged; dividing by zero clears C,
; leaves the other flags alone and raises vector 5 with a format 2 frame
; (stacked PC = next instruction, address = this instruction). Any (An)+ /
; -(An) update of the source EA happens before the zero check.

; ---------------------------------------------------------------------------
; DIVU.W / DIVS.W <ea>,Dn: 32 / 16 -> remainder:quotient in Dn
; ---------------------------------------------------------------------------
divw:
        call    src_word                ; x10 = divisor word (zero-extended)
        b.eq    x10, x0, div_zero
        srli    x8, x1, 9
        andi    x8, x8, 7
        rdg     x11, x8
        andi    x5, x1, 0x100
        b.ne    x5, x0, divw_s
        divu.16 x11, x10
        wrg     x8, x11
        jal     x0, exit
divw_s:
        divs.16 x11, x10
        wrg     x8, x11
        jal     x0, exit

div_zero:
        rdccr   x5
        andi    x5, x5, 0x1e            ; C = 0
        wrccr   x5
        add     gpc, x4, x0
        gfault  5

div_overflow:
        rdccr   x5
        andi    x5, x5, 0x1e            ; C = 0
        ori     x5, x5, 2               ; V = 1
        wrccr   x5
        jal     x0, exit

; ---------------------------------------------------------------------------
; DIVU.L / DIVS.L <ea>,Dq  (32/32)  and  <ea>,Dr:Dq  (64/32)
; Extension word: Dq in 14:12, signed in bit 11, 64-bit dividend in bit 10,
; Dr in 2:0. Results: Dr = remainder, then Dq = quotient (so Dq wins when
; Dr = Dq, the 32-bit form).
; ---------------------------------------------------------------------------
divl:
        srli    x10, x2, 16             ; extension word
        srli    x8, x10, 12
        andi    x8, x8, 7               ; Dq
        andi    x9, x10, 7              ; Dr
        addi    x21, x0, 4
        call    src_long                ; x11 = divisor
        call    commit_an
        b.eq    x11, x0, div_zero
        rdg     x12, x8                 ; dividend low
        andi    x5, x10, 0x400
        andi    x6, x10, 0x800
        b.ne    x5, x0, divl64
        b.ne    x6, x0, divl32s
        divu    x13, x12, x11
        remu    x14, x12, x11
        jal     x0, divl_done
divl32s:
        lui     x7, 0x80000             ; 0x80000000 / -1 overflows
        b.ne    x12, x7, divl32s_ok
        addi    x7, x0, -1
        b.eq    x11, x7, div_overflow
divl32s_ok:
        div     x13, x12, x11
        rem     x14, x12, x11
        jal     x0, divl_done

divl64:
        rdg     x13, x9                 ; dividend high
        b.ne    x6, x0, divl64s
        b.ne    x13, x0, divl64u_big
        divu    x13, x12, x11
        remu    x14, x12, x11
        jal     x0, divl_done
divl64u_big:
        b.geu   x13, x11, div_overflow  ; quotient would not fit in 32 bits
        call    udiv64
        add     x14, x13, x0
        add     x13, x12, x0
        jal     x0, divl_done

; signed 64/32: divide magnitudes, then fix the signs
divl64s:
        slt     x15, x13, x0            ; dividend negative
        b.eq    x15, x0, divl64s_dpos
        sub     x12, x0, x12            ; negate high:low
        orn     x13, x0, x13
        b.ne    x12, x0, divl64s_dpos
        addi    x13, x13, 1
divl64s_dpos:
        slt     x7, x11, x0             ; divisor negative
        b.eq    x7, x0, divl64s_vpos
        sub     x11, x0, x11
divl64s_vpos:
        xor     x7, x7, x15             ; quotient negative
        b.ne    x13, x0, divl64s_big
        divu    x13, x12, x11
        remu    x14, x12, x11
        jal     x0, divl64s_sign
divl64s_big:
        b.geu   x13, x11, div_overflow
        call    udiv64
        add     x14, x13, x0
        add     x13, x12, x0
divl64s_sign:
        lui     x6, 0x80000
        b.eq    x7, x0, divl64s_qpos
        b.ltu   x6, x13, div_overflow   ; |q| > 0x80000000
        sub     x13, x0, x13
        jal     x0, divl64s_rem
divl64s_qpos:
        b.geu   x13, x6, div_overflow   ; q > 0x7fffffff
divl64s_rem:
        b.eq    x15, x0, divl_done      ; remainder takes the dividend's sign
        sub     x14, x0, x14

divl_done:                              ; x13 = quotient, x14 = remainder
        wrg     x9, x14
        wrg     x8, x13
        rdccr   x5
        andi    x5, x5, 0x10            ; keep X; V = C = 0
        b.ne    x13, x0, divl_nz
        ori     x5, x5, 4
divl_nz:
        b.ge    x13, x0, divl_pos
        ori     x5, x5, 8
divl_pos:
        wrccr   x5
        jal     x0, exit

; Unsigned 64/32 restoring division, high < divisor:
; x13:x12 / x11  ->  x12 = quotient, x13 = remainder. Scratch x18-x18.
udiv64:
        addi    x16, x0, 32
udiv64_loop:
        srli    x17, x13, 31
        slli    x13, x13, 1
        srli    x18, x12, 31
        or      x13, x13, x18
        slli    x12, x12, 1
        b.ne    x17, x0, udiv64_sub
        b.ltu   x13, x11, udiv64_next
udiv64_sub:
        sub     x13, x13, x11
        ori     x12, x12, 1
udiv64_next:
        addi    x16, x16, -1
        b.ne    x16, x0, udiv64_loop
        ret
