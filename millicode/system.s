; Millicode for 68040 system-state instructions.
;
; Entry (spec/uisa.yaml, entry point `insn`):
;   x1 = opword, x2 = extension words 0:1 (word 0 in bits 31:16),
;   x3 = extension words 2:3, x4 = address of the next sequential instruction.
; gpc holds the instruction's own address; gsr holds the SR system byte
; (bits 15:8); the CCR is the renamed flags (rdccr / wrccr).
; Guest registers are reached through rdg / wrg with index 0-7 = D0-D7,
; 8-15 = A0-A7. Exit by writing the next PC to gpc and executing gexit.
;
; Forms not handled yet stop at `brk`, which the shadow model reports.

; ---------------------------------------------------------------------------
; MOVE from SR / from CCR / to CCR / to SR, and MOVE USP
; ---------------------------------------------------------------------------
move_sr:
        srli    x5, x1, 8
        addi    x6, x0, 0x40
        b.eq    x5, x6, from_sr
        addi    x6, x0, 0x42
        b.eq    x5, x6, from_ccr
        addi    x6, x0, 0x44
        b.eq    x5, x6, to_ccr
        addi    x6, x0, 0x46
        b.eq    x5, x6, to_sr
        addi    x6, x0, 0x4e
        b.eq    x5, x6, move_usp
        brk     1

from_sr:
        andi    x9, gsr, 0x2000         ; privileged
        b.eq    x9, x0, privilege
        rdccr   x10
        or      x10, x10, gsr           ; full SR
        jal     x0, store_word_dn

from_ccr:
        rdccr   x10
        jal     x0, store_word_dn

; Write the low word of x10 to the opword's EA (Dn or memory)
store_word_dn:
        srli    x7, x1, 3
        andi    x7, x7, 7
        b.ne    x7, x0, store_word_mem
        andi    x8, x1, 7
        rdg     x11, x8
        mrg16   x11, x11, x10
        wrg     x8, x11
        jal     x0, exit
store_word_mem:
        andi    x20, x1, 0x3f
        addi    x21, x0, 2
        addi    x22, x0, 2
        call    ea_addr
        st.16.gd x10, 0(x23)
        call    commit_an
        jal     x0, exit

to_ccr:
        call    src_word
        wrccr   x10
        jal     x0, exit

to_sr:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        call    src_word
        call    set_sr
        jal     x0, exit

move_usp:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        andi    x8, x1, 7
        addi    x8, x8, 8               ; An index
        andi    x5, x1, 8
        b.ne    x5, x0, usp_to_an
        rdg     x10, x8
        csrw    usp, x10
        jal     x0, exit
usp_to_an:
        csrr    x10, usp
        wrg     x8, x10
        jal     x0, exit

; x10 = source word of a MOVE to SR/CCR: Dn.W, #imm or memory
src_word:
        srli    x7, x1, 3
        andi    x7, x7, 7
        andi    x8, x1, 7
        b.eq    x7, x0, src_dn
        andi    x20, x1, 0x3f
        addi    x6, x0, 0x3c
        b.ne    x20, x6, src_mem
        srli    x10, x2, 16
        ret
src_mem:
        addi    x21, x0, 2
        addi    x22, x0, 2
        call    ea_addr
        ld.16u.gd x10, 0(x23)
        call    commit_an
        ret
src_dn:
        rdg     x10, x8
        zext16  x10, x10
        ret

; Load SR from x10: flags, system byte, and switch stacks if S changes.
set_sr:
        wrccr   x10
        lui     x12, 0xf
        ori     x12, x12, 0x700         ; T1 T0 S M - I2 I1 I0
        and     x11, x10, x12
        xor     x13, x11, gsr
        andi    x13, x13, 0x1000        ; changing M: not handled yet
        b.ne    x13, x0, unsupported
        andi    x15, gsr, 0x1000        ; M = 1 (master stack): not handled yet
        b.ne    x15, x0, unsupported
        andi    x13, gsr, 0x2000        ; old S
        andi    x14, x11, 0x2000        ; new S
        b.eq    x13, x14, set_sr_done
        b.eq    x14, x0, set_sr_user
        csrw    usp, a7                 ; user -> supervisor
        csrr    a7, isp
        jal     x0, set_sr_done
set_sr_user:
        csrw    isp, a7                 ; supervisor -> user
        csrr    a7, usp
set_sr_done:
        add     gsr, x11, x0
        ret

; ---------------------------------------------------------------------------
; ORI / ANDI / EORI #imm to CCR (byte) or SR (word, privileged)
; ---------------------------------------------------------------------------
logic_sr:
        srli    x10, x2, 16             ; immediate word
        rdccr   x11
        or      x11, x11, gsr           ; current SR
        andi    x13, x1, 0x40           ; set: SR form
        b.eq    x13, x0, logic_ccr_imm
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        jal     x0, logic_op
logic_ccr_imm:
        andi    x10, x10, 0xff
logic_op:
        srli    x6, x1, 9
        andi    x6, x6, 7               ; 0 = OR, 1 = AND, 5 = EOR
        b.eq    x6, x0, logic_or
        addi    x7, x0, 1
        b.eq    x6, x7, logic_and
        xor     x12, x11, x10
        jal     x0, logic_done
logic_or:
        or      x12, x11, x10
        jal     x0, logic_done
logic_and:
        b.ne    x13, x0, logic_and_sr
        lui     x7, 0xf
        ori     x7, x7, 0xf00           ; CCR form leaves the system byte alone
        or      x10, x10, x7
logic_and_sr:
        and     x12, x11, x10
logic_done:
        b.ne    x13, x0, logic_set_sr
        wrccr   x12
        jal     x0, exit
logic_set_sr:
        add     x10, x12, x0
        call    set_sr
        jal     x0, exit

; ---------------------------------------------------------------------------
; MOVEC Rc,Rn (4e7a) and MOVEC Rn,Rc (4e7b)
; ---------------------------------------------------------------------------
movec:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        srli    x10, x2, 16             ; extension word
        srli    x8, x10, 12             ; general register: 0-7 Dn, 8-15 An
        andi    x9, x10, 0xfff          ; control register code
        andi    x5, x1, 1
        b.ne    x5, x0, movec_to
        call    ctl_read
        wrg     x8, x11
        jal     x0, exit
movec_to:
        rdg     x11, x8
        call    ctl_write
        jal     x0, exit

; x11 = control register x9
ctl_read:
        b.eq    x9, x0, rd_sfc
        addi    x6, x0, 1
        b.eq    x9, x6, rd_dfc
        addi    x6, x0, 2
        b.eq    x9, x6, rd_cacr
        addi    x6, x0, 3
        b.eq    x9, x6, rd_tc
        addi    x6, x0, 4
        b.eq    x9, x6, rd_itt0
        addi    x6, x0, 5
        b.eq    x9, x6, rd_itt1
        addi    x6, x0, 6
        b.eq    x9, x6, rd_dtt0
        addi    x6, x0, 7
        b.eq    x9, x6, rd_dtt1
        addi    x6, x0, 0x800
        b.eq    x9, x6, rd_usp
        addi    x6, x0, 0x801
        b.eq    x9, x6, rd_vbr
        addi    x6, x0, 0x803
        b.eq    x9, x6, rd_msp
        addi    x6, x0, 0x804
        b.eq    x9, x6, rd_isp
        addi    x6, x0, 0x805
        b.eq    x9, x6, rd_mmusr
        addi    x6, x0, 0x806
        b.eq    x9, x6, rd_urp
        addi    x6, x0, 0x807
        b.eq    x9, x6, rd_srp
        jal     x0, illegal
rd_sfc:
        csrr    x11, sfc
        ret
rd_dfc:
        csrr    x11, dfc
        ret
rd_cacr:
        csrr    x11, cacr
        ret
rd_tc:
        csrr    x11, tc
        ret
rd_itt0:
        csrr    x11, itt0
        ret
rd_itt1:
        csrr    x11, itt1
        ret
rd_dtt0:
        csrr    x11, dtt0
        ret
rd_dtt1:
        csrr    x11, dtt1
        ret
rd_usp:
        csrr    x11, usp
        ret
rd_vbr:
        csrr    x11, vbr
        ret
rd_msp:
        andi    x5, gsr, 0x1000         ; M set: MSP is the active a7
        b.ne    x5, x0, rd_a7
        csrr    x11, msp
        ret
rd_isp:
        andi    x5, gsr, 0x1000         ; M clear: ISP is the active a7
        b.eq    x5, x0, rd_a7
        csrr    x11, isp
        ret
rd_a7:
        add     x11, a7, x0
        ret
rd_mmusr:
        csrr    x11, mmusr
        ret
rd_urp:
        csrr    x11, urp
        ret
rd_srp:
        csrr    x11, srp
        ret

; control register x9 = x11
ctl_write:
        b.eq    x9, x0, wr_sfc
        addi    x6, x0, 1
        b.eq    x9, x6, wr_dfc
        addi    x6, x0, 2
        b.eq    x9, x6, wr_cacr
        addi    x6, x0, 3
        b.eq    x9, x6, wr_tc
        addi    x6, x0, 4
        b.eq    x9, x6, wr_itt0
        addi    x6, x0, 5
        b.eq    x9, x6, wr_itt1
        addi    x6, x0, 6
        b.eq    x9, x6, wr_dtt0
        addi    x6, x0, 7
        b.eq    x9, x6, wr_dtt1
        addi    x6, x0, 0x800
        b.eq    x9, x6, wr_usp
        addi    x6, x0, 0x801
        b.eq    x9, x6, wr_vbr
        addi    x6, x0, 0x803
        b.eq    x9, x6, wr_msp
        addi    x6, x0, 0x804
        b.eq    x9, x6, wr_isp
        addi    x6, x0, 0x805
        b.eq    x9, x6, wr_mmusr
        addi    x6, x0, 0x806
        b.eq    x9, x6, wr_urp
        addi    x6, x0, 0x807
        b.eq    x9, x6, wr_srp
        jal     x0, illegal
wr_sfc:
        csrw    sfc, x11
        ret
wr_dfc:
        csrw    dfc, x11
        ret
wr_cacr:
        csrw    cacr, x11
        ret
wr_tc:
        csrw    tc, x11
        ret
wr_itt0:
        csrw    itt0, x11
        ret
wr_itt1:
        csrw    itt1, x11
        ret
wr_dtt0:
        csrw    dtt0, x11
        ret
wr_dtt1:
        csrw    dtt1, x11
        ret
wr_usp:
        csrw    usp, x11
        ret
wr_vbr:
        csrw    vbr, x11
        ret
wr_msp:
        andi    x5, gsr, 0x1000
        b.ne    x5, x0, wr_a7
        csrw    msp, x11
        ret
wr_isp:
        andi    x5, gsr, 0x1000
        b.eq    x5, x0, wr_a7
        csrw    isp, x11
        ret
wr_a7:
        add     a7, x11, x0
        ret
wr_mmusr:
        csrw    mmusr, x11
        ret
wr_urp:
        csrw    urp, x11
        ret
wr_srp:
        csrw    srp, x11
        ret

; ---------------------------------------------------------------------------
; common exits
; ---------------------------------------------------------------------------
exit:
        add     gpc, x4, x0
        gexit
privilege:
        gfault  8
illegal:
        gfault  4
unsupported:
        brk     2
