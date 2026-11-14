; Millicode for TAS and the 68040 cache and MMU instructions.

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
        andi    x7, x1, 0x18
        b.eq    x7, x0, line_f          ; scope 0: not a cache instruction
        andi    x7, x1, 0xc0
        b.eq    x7, x0, exit            ; no caches selected: no operation
        srli    x5, x1, 3
        andi    x5, x5, 31              ; cache:push:scope

        addi    x7, x0, 9
        b.eq    x5, x7, cache_inv1_1
        addi    x7, x0, 10
        b.eq    x5, x7, cache_inv1_2
        addi    x7, x0, 11
        b.eq    x5, x7, cache_inv1_3
        addi    x7, x0, 13
        b.eq    x5, x7, cache_push1_1
        addi    x7, x0, 14
        b.eq    x5, x7, cache_push1_2
        addi    x7, x0, 15
        b.eq    x5, x7, cache_push1_3
        addi    x7, x0, 17
        b.eq    x5, x7, cache_inv2_1
        addi    x7, x0, 18
        b.eq    x5, x7, cache_inv2_2
        addi    x7, x0, 19
        b.eq    x5, x7, cache_inv2_3
        addi    x7, x0, 21
        b.eq    x5, x7, cache_push2_1
        addi    x7, x0, 22
        b.eq    x5, x7, cache_push2_2
        addi    x7, x0, 23
        b.eq    x5, x7, cache_push2_3
        addi    x7, x0, 25
        b.eq    x5, x7, cache_inv3_1
        addi    x7, x0, 26
        b.eq    x5, x7, cache_inv3_2
        addi    x7, x0, 27
        b.eq    x5, x7, cache_inv3_3
        addi    x7, x0, 29
        b.eq    x5, x7, cache_push3_1
        addi    x7, x0, 30
        b.eq    x5, x7, cache_push3_2
        addi    x7, x0, 31
        b.eq    x5, x7, cache_push3_3
        jal     x0, illegal
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
; PTESTW (An) / PTESTR (An) (privileged): MMUSR = the table walker's result
; ---------------------------------------------------------------------------
ptest_insn:
        andi    x9, gsr, 0x2000
        b.eq    x9, x0, privilege
        andi    x8, x1, 7
        addi    x8, x8, 8
        rdg     x6, x8
        andi    x5, x1, 0x20            ; set: PTESTR
        b.ne    x5, x0, ptestr
        ptest   x7, x6, 1
        jal     x0, ptest_done
ptestr:
        ptest   x7, x6, 0
ptest_done:
        csrw    mmusr, x7
        jal     x0, exit

