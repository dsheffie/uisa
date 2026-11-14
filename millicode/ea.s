; Effective-address helpers shared by millicode routines.
;
; get_ext   x22 = extension-word offset (2, 4, 6 or 8)  ->  x18 = that word
; ea_addr   x20 = 6-bit EA field (mode << 3 | reg), x21 = operand size in
;           bytes, x22 = offset of the EA's first extension word
;           ->  x23 = address. For (An)+ / -(An): x24 = updated An, x25 = its
;           rdg index, x26 = 1. The caller commits the update with
;           `call commit_an` after its memory accesses, so a fault leaves
;           An unchanged (faults before commits).
; Scratch: x16-x19 (and x24-x26 for full-format index words, which also
; correct x4 for the extra length). Nests at most three calls deep.

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
        rdg     x23, x25
        jal     x0, ea_index            ; tail call: returns to ea_addr's caller
; x23 = base (An, or the extension word's address for PC modes) -> x23 =
; address, from the index extension word at x22 (brief or full format)
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
        add     x23, x23, x18
        add     x23, x23, x17
        ret

; Full-format extension word (68020+): base displacement, suppressible base
; and index, optional memory indirection (pre- or post-indexed) with an outer
; displacement. x4 is corrected for the extra length: the dispatch hardware
; computes it from the opword alone, assuming a brief-format word. Uses
; x24-x26 as scratch (not live for indexed modes; x26 is left 0 for
; commit_an).
ea_full:
        add     x24, x18, x0            ; extension word
        add     x25, x22, x0            ; its offset
        andi    x16, x24, 0x08          ; reserved encodings
        b.ne    x16, x0, ea_full_bad
        andi    x16, x24, 7
        addi    x26, x0, 4
        b.eq    x16, x26, ea_full_bad
        andi    x26, x24, 0x40
        b.eq    x26, x0, ea_full_ok
        addi    x26, x0, 3              ; IS = 1: only I/IS 0-3
        b.ltu   x26, x16, ea_full_bad
ea_full_ok:
        add     x17, x0, x0             ; index * scale, or 0 if suppressed (IS)
        andi    x16, x24, 0x40
        b.ne    x16, x0, ea_full_base
        srli    x16, x24, 12
        rdg     x17, x16
        andi    x16, x24, 0x800
        b.ne    x16, x0, ea_full_xl
        sext16  x17, x17
ea_full_xl:
        srli    x16, x24, 9
        andi    x16, x16, 3
        sll     x17, x17, x16
ea_full_base:
        andi    x16, x24, 0x80          ; BS: base suppressed
        b.eq    x16, x0, ea_full_bd
        add     x23, x0, x0
ea_full_bd:
        addi    x22, x22, 2
        srli    x16, x24, 4
        andi    x16, x16, 3             ; base displacement: 1 null, 2 word, 3 long
        b.eq    x16, x0, ea_full_bad
        addi    x26, x0, 2
        b.eq    x16, x26, ea_full_bdw
        addi    x26, x0, 3
        b.ne    x16, x26, ea_full_ind
        call    get_ext
        slli    x16, x18, 16
        addi    x22, x22, 2
        call    get_ext
        or      x16, x16, x18
        add     x23, x23, x16
        addi    x22, x22, 2
        jal     x0, ea_full_ind
ea_full_bdw:
        call    get_ext
        sext16  x18, x18
        add     x23, x23, x18
        addi    x22, x22, 2
ea_full_ind:                            ; x23 = base + bd
        andi    x16, x24, 7             ; I/IS
        b.ne    x16, x0, ea_full_mem
        add     x23, x23, x17
        jal     x0, ea_full_done
ea_full_mem:
        andi    x26, x16, 4
        b.ne    x26, x0, ea_full_post
        add     x23, x23, x17           ; pre-indexed
        ld.32.gd x23, 0(x23)
        jal     x0, ea_full_od
ea_full_post:
        ld.32.gd x23, 0(x23)            ; post-indexed
        add     x23, x23, x17
ea_full_od:
        andi    x16, x24, 3             ; outer displacement: 1 null, 2 word, 3 long
        addi    x26, x0, 2
        b.eq    x16, x26, ea_full_odw
        addi    x26, x0, 3
        b.ne    x16, x26, ea_full_done
        call    get_ext
        slli    x16, x18, 16
        addi    x22, x22, 2
        call    get_ext
        or      x16, x16, x18
        add     x23, x23, x16
        addi    x22, x22, 2
        jal     x0, ea_full_done
ea_full_odw:
        call    get_ext
        sext16  x18, x18
        add     x23, x23, x18
        addi    x22, x22, 2
ea_full_done:
        sub     x16, x22, x25           ; extension bytes after the full-format word
        addi    x16, x16, -2
        add     x4, x4, x16
        add     x22, x25, x0
        add     x26, x0, x0
        ret
ea_full_bad:
        brk     5                       ; reserved full-format encoding
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
        add     x23, gpc, x22
        jal     x0, ea_index

commit_an:
        b.eq    x26, x0, commit_an_done
        wrg     x25, x24
commit_an_done:
        ret
