; Millicode for the bit-field instructions that write their field:
; BFCHG / BFCLR / BFSET <ea>{offset:width} and BFINS Dn,<ea>{offset:width}.
;
; Flags: N and Z from the old field (BFCHG/BFCLR/BFSET) or from the inserted
; value (BFINS); V = C = 0; X unchanged. A Dn field wraps around the register
; (offset mod 32). A memory field starts offset bits (signed, from Dn) past
; the EA byte and touches 1-5 bytes, all of which are read and rewritten.

bfwrite:
        srli    x10, x2, 16             ; extension word
        andi    x5, x10, 0x800
        b.ne    x5, x0, bw_off_reg
        srli    x11, x10, 6
        andi    x11, x11, 31            ; immediate offset
        jal     x0, bw_width
bw_off_reg:
        srli    x5, x10, 6
        andi    x5, x5, 7
        rdg     x11, x5                 ; offset from Dn (signed for memory)
bw_width:
        andi    x5, x10, 0x20
        b.ne    x5, x0, bw_wid_reg
        andi    x12, x10, 31
        jal     x0, bw_wid_fix
bw_wid_reg:
        andi    x5, x10, 7
        rdg     x12, x5
        andi    x12, x12, 31
bw_wid_fix:
        b.ne    x12, x0, bw_ea
        addi    x12, x0, 32
bw_ea:
        srli    x5, x1, 3
        andi    x5, x5, 7
        b.ne    x5, x0, bw_mem
        andi    x9, x1, 7
        rdg     x14, x9
        andi    x7, x11, 31
        rol     x13, x14, x7            ; field now starts at bit 31
        call    bw_op
        ror     x13, x13, x7
        wrg     x9, x13
        jal     x0, exit

bw_mem:
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
        addi    x24, x8, 0
        addi    x6, x0, 4
        b.geu   x6, x24, bw_ld
        addi    x24, x0, 4              ; x24 = bytes in the first word (<= 4)
bw_ld:                                  ; x14 = bytes 0-3, left-aligned; x15 = byte 4
        add     x14, x0, x0
        add     x15, x0, x0
        add     x9, x0, x0
        addi    x25, x0, 24
bw_ld_loop:
        b.eq    x9, x24, bw_ld_done
        add     x6, x23, x9
        ld.8u.gd x16, 0(x6)
        sll     x16, x16, x25
        or      x14, x14, x16
        addi    x25, x25, -8
        addi    x9, x9, 1
        jal     x0, bw_ld_loop
bw_ld_done:
        addi    x6, x0, 5
        b.ne    x8, x6, bw_have
        ld.8u.gd x15, 4(x23)
bw_have:
        sll     x13, x14, x7            ; field at bit 31
        b.eq    x7, x0, bw_have2
        addi    x6, x0, 8
        sub     x6, x6, x7
        srl     x16, x15, x6
        or      x13, x13, x16
bw_have2:
        call    bw_op                   ; x13 = new field at bit 31; x17 = field mask there
        and     x13, x13, x17
        srl     x16, x17, x7            ; first-word mask and bits
        andn    x14, x14, x16
        srl     x16, x13, x7
        or      x14, x14, x16
        addi    x6, x0, 5
        b.ne    x8, x6, bw_st           ; byte 4 (x7 >= 1 here)
        addi    x6, x0, 32
        sub     x6, x6, x7
        sll     x16, x17, x6
        srli    x16, x16, 24
        andn    x15, x15, x16
        sll     x16, x13, x6
        srli    x16, x16, 24
        or      x15, x15, x16
        st.8.gd x15, 4(x23)
bw_st:
        add     x9, x0, x0
        addi    x25, x0, 24
bw_st_loop:
        b.eq    x9, x24, exit
        srl     x16, x14, x25
        add     x6, x23, x9
        st.8.gd x16, 0(x6)
        addi    x25, x25, -8
        addi    x9, x9, 1
        jal     x0, bw_st_loop

; x13 = word with the field at bit 31, x12 = width. Sets the flags and
; returns x13 with the field changed and x17 = the field mask (top x12 bits).
; Scratch x5, x6, x16.
bw_op:
        addi    x17, x0, -1
        addi    x6, x0, 32
        sub     x6, x6, x12
        sll     x17, x17, x6            ; field mask (width 32: shift 0)
        srli    x5, x1, 8
        andi    x5, x5, 7               ; ea chg, ec clr, ee set, ef ins
        and     x16, x13, x17           ; old field: flags for chg/clr/set
        addi    x6, x0, 7
        b.ne    x5, x6, bw_flags
        srli    x16, x10, 12
        andi    x16, x16, 7
        rdg     x16, x16                ; BFINS source: low x12 bits of Dn
        addi    x6, x0, 32
        sub     x6, x6, x12
        sll     x16, x16, x6            ; inserted value at bit 31
bw_flags:
        rdccr   x6
        andi    x6, x6, 0x10            ; keep X; V = C = 0
        b.ne    x16, x0, bw_nz
        ori     x6, x6, 4
bw_nz:
        b.ge    x16, x0, bw_pos
        ori     x6, x6, 8
bw_pos:
        wrccr   x6
        addi    x6, x0, 2
        b.eq    x5, x6, bw_chg
        addi    x6, x0, 4
        b.eq    x5, x6, bw_clr
        addi    x6, x0, 6
        b.eq    x5, x6, bw_set
        andn    x13, x13, x17           ; BFINS
        or      x13, x13, x16
        ret
bw_chg:
        xor     x13, x13, x17
        ret
bw_clr:
        andn    x13, x13, x17
        ret
bw_set:
        or      x13, x13, x17
        ret
