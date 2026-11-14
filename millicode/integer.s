; Millicode for integer instructions the PLA does not handle.
; Entry convention as in system.s.

; ---------------------------------------------------------------------------
; DBcc Dn,<disp> (DBF is a PLA flow)
; ---------------------------------------------------------------------------
dbcc:
        srli    x5, x1, 8
        andi    x5, x5, 15
        call    cond_eval
        b.ne    x10, x0, exit           ; condition true: fall through
        andi    x8, x1, 7
        rdg     x11, x8
        addi    x12, x11, -1
        mrg16   x11, x11, x12           ; Dn.W -= 1
        wrg     x8, x11
        zext16  x12, x12
        addi    x13, x0, 1
        slli    x13, x13, 16
        addi    x13, x13, -1            ; 0xffff
        b.eq    x12, x13, exit          ; count expired
        srli    x14, x2, 16
        sext16  x14, x14
        addi    x14, x14, 2
        add     gpc, gpc, x14
        gexit

; x10 = nonzero if guest condition x5 holds
cond_eval:
        b.eq    x5, x0, cc_t
        addi    x6, x0, 1
        b.eq    x5, x6, cc_f
        addi    x6, x0, 2
        b.eq    x5, x6, cc_hi
        addi    x6, x0, 3
        b.eq    x5, x6, cc_ls
        addi    x6, x0, 4
        b.eq    x5, x6, cc_cc
        addi    x6, x0, 5
        b.eq    x5, x6, cc_cs
        addi    x6, x0, 6
        b.eq    x5, x6, cc_ne
        addi    x6, x0, 7
        b.eq    x5, x6, cc_eq
        addi    x6, x0, 8
        b.eq    x5, x6, cc_vc
        addi    x6, x0, 9
        b.eq    x5, x6, cc_vs
        addi    x6, x0, 10
        b.eq    x5, x6, cc_pl
        addi    x6, x0, 11
        b.eq    x5, x6, cc_mi
        addi    x6, x0, 12
        b.eq    x5, x6, cc_ge
        addi    x6, x0, 13
        b.eq    x5, x6, cc_lt
        addi    x6, x0, 14
        b.eq    x5, x6, cc_gt
        s.le    x10
        ret
cc_t:
        s.t     x10
        ret
cc_f:
        s.f     x10
        ret
cc_hi:
        s.hi    x10
        ret
cc_ls:
        s.ls    x10
        ret
cc_cc:
        s.cc    x10
        ret
cc_cs:
        s.cs    x10
        ret
cc_ne:
        s.ne    x10
        ret
cc_eq:
        s.eq    x10
        ret
cc_vc:
        s.vc    x10
        ret
cc_vs:
        s.vs    x10
        ret
cc_pl:
        s.pl    x10
        ret
cc_mi:
        s.mi    x10
        ret
cc_ge:
        s.ge    x10
        ret
cc_lt:
        s.lt    x10
        ret
cc_gt:
        s.gt    x10
        ret

; ---------------------------------------------------------------------------
; CMPM (Ay)+,(Ax)+
; TODO: the Ay update is committed before the (Ax) load; reorder so that both
; loads happen first (faults before commits).
; ---------------------------------------------------------------------------
cmpm:
        srli    x5, x1, 6
        andi    x5, x5, 3               ; size: 0 = B, 1 = W, 2 = L
        andi    x8, x1, 7
        addi    x8, x8, 8               ; Ay
        srli    x9, x1, 9
        andi    x9, x9, 7
        addi    x9, x9, 8               ; Ax
        addi    x6, x0, 1
        sll     x21, x6, x5             ; size in bytes
        add     x25, x8, x0
        call    ea_incr                 ; x17 = Ay step
        rdg     x10, x8
        call    cmpm_load
        add     x12, x11, x0            ; source operand
        add     x10, x10, x17
        wrg     x8, x10
        add     x25, x9, x0
        call    ea_incr
        rdg     x10, x9
        call    cmpm_load               ; x11 = destination operand
        add     x10, x10, x17
        wrg     x9, x10
        b.eq    x5, x0, cmpm_b
        addi    x6, x0, 1
        b.eq    x5, x6, cmpm_w
        cmp.32  x11, x12
        jal     x0, exit
cmpm_w:
        cmp.16  x11, x12
        jal     x0, exit
cmpm_b:
        cmp.8   x11, x12
        jal     x0, exit
; x11 = operand of size x5 at x10
cmpm_load:
        b.eq    x5, x0, cmpm_load_b
        addi    x6, x0, 1
        b.eq    x5, x6, cmpm_load_w
        ld.32.gd x11, 0(x10)
        ret
cmpm_load_w:
        ld.16u.gd x11, 0(x10)
        ret
cmpm_load_b:
        ld.8u.gd x11, 0(x10)
        ret

; ---------------------------------------------------------------------------
; MULU.L / MULS.L <ea>,Dl  and  <ea>,Dh:Dl
; ---------------------------------------------------------------------------
mull:
        srli    x10, x2, 16             ; extension word
        srli    x8, x10, 12
        andi    x8, x8, 7               ; Dl
        andi    x9, x10, 7              ; Dh
        addi    x21, x0, 4
        call    src_long                ; x11 = source operand
        rdg     x12, x8
        andi    x5, x10, 0x400
        b.ne    x5, x0, mull64
        andi    x5, x10, 0x800
        b.ne    x5, x0, mull32s
        mulu.32 x13, x12, x11
        jal     x0, mull32_done
mull32s:
        muls.32 x13, x12, x11
mull32_done:
        wrg     x8, x13
        call    commit_an
        jal     x0, exit
mull64:
        mul     x13, x12, x11           ; low
        andi    x5, x10, 0x800
        b.ne    x5, x0, mull64s
        mulhu   x14, x12, x11
        jal     x0, mull64_done
mull64s:
        mulh    x14, x12, x11
mull64_done:
        wrg     x8, x13
        wrg     x9, x14
        rdccr   x15
        andi    x15, x15, 0x10          ; keep X; V = C = 0
        or      x16, x13, x14
        b.ne    x16, x0, mull64_nz
        ori     x15, x15, 4
mull64_nz:
        b.ge    x14, x0, mull64_pos
        ori     x15, x15, 8
mull64_pos:
        wrccr   x15
        call    commit_an
        jal     x0, exit

; x11 = 32-bit source operand of the opword's EA: Dn, An, #imm (extension
; words at 4 and 6) or memory (EA extension words from offset 4)
src_long:
        srli    x5, x1, 3
        andi    x5, x5, 7
        andi    x6, x1, 7
        add     x26, x0, x0
        b.eq    x5, x0, src_long_dn
        addi    x7, x0, 1
        b.eq    x5, x7, src_long_an
        andi    x20, x1, 0x3f
        addi    x7, x0, 0x3c
        b.eq    x20, x7, src_long_imm
        addi    x22, x0, 4
        call    ea_addr
        ld.32.gd x11, 0(x23)
        ret
src_long_dn:
        rdg     x11, x6
        ret
src_long_an:
        addi    x6, x6, 8
        rdg     x11, x6
        ret
src_long_imm:
        zext16  x11, x2
        slli    x11, x11, 16
        srli    x7, x3, 16
        or      x11, x11, x7
        ret

; ---------------------------------------------------------------------------
; MOVES <ea>,Rn / Rn,<ea>  (privileged; guest alternate space: SFC / DFC)
; ---------------------------------------------------------------------------
moves:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        srli    x10, x2, 16             ; extension word
        srli    x8, x10, 12             ; Rn index 0-15
        srli    x5, x1, 6
        andi    x5, x5, 3               ; size: 0 = B, 1 = W, 2 = L
        addi    x6, x0, 1
        sll     x21, x6, x5
        andi    x20, x1, 0x3f
        addi    x22, x0, 4
        call    ea_addr
        andi    x6, x10, 0x800
        b.ne    x6, x0, moves_store
        b.eq    x5, x0, moves_ld_b
        addi    x6, x0, 1
        b.eq    x5, x6, moves_ld_w
        ld.32.ga x11, 0(x23)
        jal     x0, moves_ld_done
moves_ld_w:
        ld.16u.ga x11, 0(x23)
        addi    x6, x0, 8
        b.ge    x8, x6, moves_ld_w_an
        rdg     x12, x8
        mrg16   x11, x12, x11
        jal     x0, moves_ld_done
moves_ld_w_an:
        sext16  x11, x11
        jal     x0, moves_ld_done
moves_ld_b:
        ld.8u.ga x11, 0(x23)
        addi    x6, x0, 8
        b.ge    x8, x6, moves_ld_b_an
        rdg     x12, x8
        mrg8    x11, x12, x11
        jal     x0, moves_ld_done
moves_ld_b_an:
        sext8   x11, x11
moves_ld_done:
        call    commit_an
        wrg     x8, x11
        jal     x0, exit
moves_store:
        rdg     x11, x8
        b.eq    x5, x0, moves_st_b
        addi    x6, x0, 1
        b.eq    x5, x6, moves_st_w
        st.32.ga x11, 0(x23)
        jal     x0, moves_st_done
moves_st_w:
        st.16.ga x11, 0(x23)
        jal     x0, moves_st_done
moves_st_b:
        st.8.ga x11, 0(x23)
moves_st_done:
        call    commit_an
        jal     x0, exit

; ---------------------------------------------------------------------------
; BFTST / BFEXTU / BFEXTS / BFFFO <ea>{offset:width}[,Dn]
; ---------------------------------------------------------------------------
bitfield:
        srli    x10, x2, 16             ; extension word
        andi    x5, x10, 0x800
        b.ne    x5, x0, bf_off_reg
        srli    x11, x10, 6
        andi    x11, x11, 31            ; immediate offset
        jal     x0, bf_width
bf_off_reg:
        srli    x5, x10, 6
        andi    x5, x5, 7
        rdg     x11, x5                 ; offset from Dn (signed for memory)
bf_width:
        andi    x5, x10, 0x20
        b.ne    x5, x0, bf_wid_reg
        andi    x12, x10, 31
        jal     x0, bf_wid_fix
bf_wid_reg:
        andi    x5, x10, 7
        rdg     x12, x5
        andi    x12, x12, 31
bf_wid_fix:
        b.ne    x12, x0, bf_ea
        addi    x12, x0, 32
bf_ea:
        srli    x5, x1, 3
        andi    x5, x5, 7
        b.ne    x5, x0, bf_mem
        andi    x6, x1, 7
        rdg     x14, x6
        andi    x7, x11, 31
        rol     x13, x14, x7            ; field now starts at bit 31
        jal     x0, bf_have
bf_mem:
        andi    x20, x1, 0x3f
        addi    x21, x0, 1
        addi    x22, x0, 4
        call    ea_addr
        srai    x6, x11, 3
        add     x23, x23, x6            ; first byte of the field
        andi    x7, x11, 7              ; bit offset within it
        add     x8, x7, x12
        addi    x8, x8, 7
        srli    x8, x8, 3               ; bytes touched: 1-5
        add     x13, x0, x0
        add     x9, x0, x0
bf_ld:
        addi    x6, x0, 4
        b.eq    x9, x6, bf_ld4
        b.eq    x9, x8, bf_ld_done
        add     x6, x23, x9
        ld.8u.gd x14, 0(x6)
        slli    x13, x13, 8
        or      x13, x13, x14
        addi    x9, x9, 1
        jal     x0, bf_ld
bf_ld_done:
        addi    x6, x0, 4
        sub     x6, x6, x8
        slli    x6, x6, 3
        sll     x13, x13, x6
        sll     x13, x13, x7
        jal     x0, bf_have
bf_ld4:
        sll     x13, x13, x7
        addi    x6, x0, 5
        b.ne    x8, x6, bf_have
        ld.8u.gd x14, 4(x23)
        addi    x6, x0, 8
        sub     x6, x6, x7
        srl     x14, x14, x6
        or      x13, x13, x14
bf_have:                                ; x13 = field from bit 31; x12 = width; x11 = offset
        addi    x6, x0, 32
        sub     x6, x6, x12
        srl     x15, x13, x6            ; zero-extended field
        rdccr   x16
        andi    x16, x16, 0x10          ; keep X; V = C = 0
        b.ne    x15, x0, bf_nz
        ori     x16, x16, 4
bf_nz:
        b.ge    x13, x0, bf_pos
        ori     x16, x16, 8
bf_pos:
        wrccr   x16
        srli    x9, x10, 12
        andi    x9, x9, 7               ; result register
        srli    x5, x1, 8
        andi    x5, x5, 7               ; e8 bftst, e9 bfextu, eb bfexts, ed bfffo
        b.eq    x5, x0, exit
        addi    x6, x0, 1
        b.eq    x5, x6, bf_extu
        addi    x6, x0, 3
        b.eq    x5, x6, bf_exts
        addi    x6, x0, 5
        b.eq    x5, x6, bf_ffo
        brk     7
bf_extu:
        wrg     x9, x15
        jal     x0, exit
bf_exts:
        addi    x6, x0, 32
        sub     x6, x6, x12
        sra     x15, x13, x6
        wrg     x9, x15
        jal     x0, exit
bf_ffo:
        clz     x14, x13
        minu    x14, x14, x12           ; no 1 bit in the field: offset + width
        add     x14, x14, x11
        wrg     x9, x14
        jal     x0, exit
