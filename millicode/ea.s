; Effective-address helpers shared by millicode routines.
;
; get_ext   x22 = extension-word offset (2, 4, 6 or 8)  ->  x18 = that word
; ea_addr   x20 = 6-bit EA field (mode << 3 | reg), x21 = operand size in
;           bytes, x22 = offset of the EA's first extension word
;           ->  x23 = address. For (An)+ / -(An): x24 = updated An, x25 = its
;           rdg index, x26 = 1. The caller commits the update with
;           `call commit_an` after its memory accesses, so a fault leaves
;           An unchanged (faults before commits).
; Scratch: x16-x19. Nests at most three calls deep.

get_ext:
        addi    x19, x0, 2
        b.eq    x22, x19, get_ext2
        addi    x19, x0, 4
        b.eq    x22, x19, get_ext4
        addi    x19, x0, 6
        b.eq    x22, x19, get_ext6
        addi    x19, x0, 8
        b.eq    x22, x19, get_ext8
        brk     3
get_ext2:
        srli    x18, x2, 16
        ret
get_ext4:
        zext16  x18, x2
        ret
get_ext6:
        srli    x18, x3, 16
        ret
get_ext8:
        zext16  x18, x3
        ret

ea_addr:
        add     x26, x0, x0
        andi    x25, x20, 7
        addi    x25, x25, 8             ; An index for modes 2-6
        srli    x16, x20, 3
        andi    x16, x16, 7
        addi    x17, x0, 2
        b.eq    x16, x17, ea_ind
        addi    x17, x0, 3
        b.eq    x16, x17, ea_postinc
        addi    x17, x0, 4
        b.eq    x16, x17, ea_predec
        addi    x17, x0, 5
        b.eq    x16, x17, ea_d16
        addi    x17, x0, 6
        b.eq    x16, x17, ea_idx
        addi    x17, x0, 7
        b.eq    x16, x17, ea_mode7
        brk     4                       ; Dn / An: not a memory operand
ea_ind:
        rdg     x23, x25
        ret
ea_postinc:
        rdg     x23, x25
        call    ea_incr
        add     x24, x23, x17
        addi    x26, x0, 1
        ret
ea_predec:
        rdg     x23, x25
        call    ea_incr
        sub     x23, x23, x17
        add     x24, x23, x0
        addi    x26, x0, 1
        ret
; x17 = the (An)+ / -(An) step: the operand size, but 2 for a byte through A7
ea_incr:
        add     x17, x21, x0
        addi    x16, x0, 1
        b.ne    x21, x16, ea_incr_done
        addi    x16, x0, 15
        b.ne    x25, x16, ea_incr_done
        addi    x17, x0, 2
ea_incr_done:
        ret
ea_d16:
        call    get_ext
        sext16  x18, x18
        rdg     x23, x25
        add     x23, x23, x18
        ret
ea_idx:
        call    ea_index
        rdg     x23, x25
        add     x23, x23, x18
        ret
; x18 = index register * scale + d8, from the brief extension word at x22
ea_index:
        call    get_ext
        andi    x16, x18, 0x100
        b.ne    x16, x0, ea_full
        srli    x16, x18, 12
        rdg     x17, x16
        andi    x16, x18, 0x800
        b.ne    x16, x0, ea_index_long
        sext16  x17, x17
ea_index_long:
        srli    x16, x18, 9
        andi    x16, x16, 3
        sll     x17, x17, x16
        sext8   x18, x18
        add     x18, x18, x17
        ret
ea_full:
        brk     5                       ; full-format extension word: not handled yet
ea_mode7:
        andi    x16, x20, 7
        b.eq    x16, x0, ea_absw
        addi    x17, x0, 1
        b.eq    x16, x17, ea_absl
        addi    x17, x0, 2
        b.eq    x16, x17, ea_pc16
        addi    x17, x0, 3
        b.eq    x16, x17, ea_pcidx
        brk     6                       ; #imm: not a memory operand
ea_absw:
        call    get_ext
        sext16  x23, x18
        ret
ea_absl:
        call    get_ext
        slli    x23, x18, 16
        addi    x22, x22, 2
        call    get_ext
        addi    x22, x22, -2
        or      x23, x23, x18
        ret
ea_pc16:
        call    get_ext
        sext16  x18, x18
        add     x23, gpc, x22
        add     x23, x23, x18
        ret
ea_pcidx:
        call    ea_index
        add     x23, gpc, x22
        add     x23, x23, x18
        ret

commit_an:
        b.eq    x26, x0, commit_an_done
        wrg     x25, x24
commit_an_done:
        ret
