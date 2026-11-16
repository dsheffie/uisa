; Millicode for TAS, CAS and the 68040 cache and MMU instructions.

; ---------------------------------------------------------------------------
; TAS <ea>: test a byte, then set its bit 7 (locked read-modify-write)
; ---------------------------------------------------------------------------
tas:
        srli    x5, x1, 3
        andi    x5, x5, 7
        b.ne    x5, x0, tas_mem
        andi    x8, x1, 7
        rdg     x11, x8
        mov.8z  x0, x11                 ; N Z from the byte, V = C = 0
        ori     x11, x11, 0x80
        wrg     x8, x11
        jal     x0, exit
tas_mem:
        andi    x20, x1, 0x3f
        addi    x21, x0, 1
        addi    x22, x0, 2
        call    ea_addr
        ldl.8.gd x11, 0(x23)
        mov.8z  x0, x11
        ori     x11, x11, 0x80
        stu.8.gd x11, 0(x23)
        call    commit_an
        jal     x0, exit

; ---------------------------------------------------------------------------
; CAS Dc,Du,<ea>: compare the operand at <ea> with Dc; if equal, store Du
; there, otherwise load the operand into Dc (a locked read-modify-write; a
; failed compare ends the sequence with unlock and writes nothing). Size in
; opword bits 10:9 (1 B, 2 W, 3 L); the extension word has Du in bits 8:6 and
; Dc in bits 2:0. The store, or the unlock, comes before any guest register
; or flag changes (faults before commits). The flags are those of CMP Dc,<ea>.
; <ea> must be memory alterable; other modes raise the illegal instruction.
; ---------------------------------------------------------------------------
cas:
        srli    x7, x1, 3
        andi    x7, x7, 7               ; EA mode
        addi    x16, x0, 2
        b.ltu   x7, x16, illegal        ; Dn, An
        addi    x16, x0, 7
        b.ne    x7, x16, cas_ea_ok
        andi    x7, x1, 7
        addi    x16, x0, 2
        b.geu   x7, x16, illegal        ; only abs.W and abs.L in mode 7
cas_ea_ok:
        srli    x5, x1, 9
        andi    x5, x5, 3               ; size code
        addi    x6, x0, 1
        addi    x7, x5, -1
        sll     x21, x6, x7             ; size in bytes: 1, 2, 4
        srli    x18, x2, 16             ; extension word
        andi    x8, x18, 7              ; Dc
        srli    x9, x18, 6
        andi    x9, x9, 7               ; Du
        rdg     x12, x8
        rdg     x13, x9
        andi    x20, x1, 0x3f
        addi    x22, x0, 4              ; the EA's words follow the extension word
        call    ea_addr                 ; x23 = address
        b.eq    x5, x6, cas_b
        addi    x7, x0, 2
        b.eq    x5, x7, cas_w
        ldl.32.gd x11, 0(x23)
        b.ne    x11, x12, cas_l_ne
        stu.32.gd x13, 0(x23)
        cmp.32  x11, x12
        jal     x0, cas_done
cas_l_ne:
        unlock
        cmp.32  x11, x12
        wrg     x8, x11
        jal     x0, cas_done
cas_w:
        ldl.16.gd x11, 0(x23)
        zext16  x14, x12
        b.ne    x11, x14, cas_w_ne
        stu.16.gd x13, 0(x23)
        cmp.16  x11, x12
        jal     x0, cas_done
cas_w_ne:
        unlock
        cmp.16  x11, x12
        srli    x14, x12, 16            ; Dc keeps its upper word
        slli    x14, x14, 16
        or      x14, x14, x11
        wrg     x8, x14
        jal     x0, cas_done
cas_b:
        ldl.8.gd x11, 0(x23)
        andi    x14, x12, 0xff
        b.ne    x11, x14, cas_b_ne
        stu.8.gd x13, 0(x23)
        cmp.8   x11, x12
        jal     x0, cas_done
cas_b_ne:
        unlock
        cmp.8   x11, x12
        srli    x14, x12, 8             ; Dc keeps its upper 24 bits
        slli    x14, x14, 8
        or      x14, x14, x11
        wrg     x8, x14
cas_done:
        call    commit_an
        jal     x0, exit

; ---------------------------------------------------------------------------
; CINVL/CINVP/CINVA and CPUSHL/CPUSHP/CPUSHA (privileged). Opword bits 7:6
; cache (1 data, 2 instruction, 3 both), bit 5 push, bits 4:3 scope (1 line,
; 2 page, 3 all), bits 2:0 An. cpush / cinv take the cache and scope in their
; immediate (bits 1:0 cache, bits 3:2 scope - 1), so each form has its own leaf.
; ---------------------------------------------------------------------------
cache:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        andi    x8, x1, 7
        addi    x8, x8, 8
        rdg     x6, x8                  ; address
        bx      x1, 3, 5                ; cache:push:scope
        jal     x0, line_f
        jal     x0, exit
        jal     x0, exit
        jal     x0, exit
        jal     x0, line_f
        jal     x0, exit
        jal     x0, exit
        jal     x0, exit
        jal     x0, line_f
        jal     x0, cache_inv1_1
        jal     x0, cache_inv1_2
        jal     x0, cache_inv1_3
        jal     x0, line_f
        jal     x0, cache_push1_1
        jal     x0, cache_push1_2
        jal     x0, cache_push1_3
        jal     x0, line_f
        jal     x0, cache_inv2_1
        jal     x0, cache_inv2_2
        jal     x0, cache_inv2_3
        jal     x0, line_f
        jal     x0, cache_push2_1
        jal     x0, cache_push2_2
        jal     x0, cache_push2_3
        jal     x0, line_f
        jal     x0, cache_inv3_1
        jal     x0, cache_inv3_2
        jal     x0, cache_inv3_3
        jal     x0, line_f
        jal     x0, cache_push3_1
        jal     x0, cache_push3_2
        jal     x0, cache_push3_3
cache_inv1_1:
        cinv    x6, 1
        jal     x0, exit
cache_inv1_2:
        cinv    x6, 5
        jal     x0, exit
cache_inv1_3:
        cinv    x6, 9
        jal     x0, exit
cache_push1_1:
        cpush   x6, 1
        jal     x0, exit
cache_push1_2:
        cpush   x6, 5
        jal     x0, exit
cache_push1_3:
        cpush   x6, 9
        jal     x0, exit
cache_inv2_1:
        cinv    x6, 2
        jal     x0, exit
cache_inv2_2:
        cinv    x6, 6
        jal     x0, exit
cache_inv2_3:
        cinv    x6, 10
        jal     x0, exit
cache_push2_1:
        cpush   x6, 2
        jal     x0, exit
cache_push2_2:
        cpush   x6, 6
        jal     x0, exit
cache_push2_3:
        cpush   x6, 10
        jal     x0, exit
cache_inv3_1:
        cinv    x6, 3
        jal     x0, exit
cache_inv3_2:
        cinv    x6, 7
        jal     x0, exit
cache_inv3_3:
        cinv    x6, 11
        jal     x0, exit
cache_push3_1:
        cpush   x6, 3
        jal     x0, exit
cache_push3_2:
        cpush   x6, 7
        jal     x0, exit
cache_push3_3:
        cpush   x6, 11
        jal     x0, exit
line_f:
        gfault  11

; ---------------------------------------------------------------------------
; PFLUSHN (An), PFLUSH (An), PFLUSHAN, PFLUSHA (privileged; DFC selects user
; or supervisor entries for the page forms)
; ---------------------------------------------------------------------------
pflush:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        andi    x8, x1, 7
        addi    x8, x8, 8
        rdg     x6, x8
        srli    x5, x1, 3
        andi    x5, x5, 3
        b.eq    x5, x0, pflushn
        addi    x7, x0, 1
        b.eq    x5, x7, pflush_page
        addi    x7, x0, 2
        b.eq    x5, x7, pflushan
        tlbflush x6, 0                  ; PFLUSHA
        jal     x0, exit
pflushn:
        tlbflush x6, 3                  ; page, non-global entries only
        jal     x0, exit
pflush_page:
        tlbflush x6, 1
        jal     x0, exit
pflushan:
        tlbflush x6, 2                  ; all non-global entries
        jal     x0, exit

; ---------------------------------------------------------------------------
; PTESTW (An) / PTESTR (An) (privileged): MMUSR = probe through DFC (.ga)
; ---------------------------------------------------------------------------
ptest_insn:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        andi    x8, x1, 7
        addi    x8, x8, 8
        rdg     x6, x8
        andi    x5, x1, 0x20            ; set: PTESTR
        b.ne    x5, x0, ptestr
        probe.w.ga x7, 0(x6)
        jal     x0, ptest_done
ptestr:
        probe.r.ga x7, 0(x6)
ptest_done:
        csrw    mmusr, x7
        jal     x0, exit

