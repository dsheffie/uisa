// Runs the generated SystemVerilog decoder over build/vectors.hex.
module tb_decode;
  import uisa_pkg::*;

  localparam int N = 4096;
  logic [31:0] words [N];
  logic [11:0] ops [N];
  int fails = 0;
  int count = 0;

  initial begin
    // vectors never use op 0 (UISA_ILLEGAL), so a zero op marks the end;
    // (two-state simulators turn 'x into 0, so 'x cannot be the sentinel)
    for (int i = 0; i < N; i++) begin
      words[i] = '0;
      ops[i] = '0;
    end
    $readmemh("build/vectors.hex", words);
    $readmemh("build/vector_ops.hex", ops);
    for (int i = 0; i < N; i++) begin
      uisa_dec_t d;
      if (ops[i] == 12'd0) break;
      d = uisa_decode(words[i]);
      count++;
      if (!d.valid || 12'(d.op) != ops[i]) begin
        $display("decode %08x: got %0d want %0d", words[i], d.op, ops[i]);
        fails++;
      end
      if (d.tie_violation) begin
        $display("tie violation reported for legal vector %08x", words[i]);
        fails++;
      end
      // uisa_attrs(op): the op's attributes without its encoding, which with
      // the fields must be exactly the decode
      begin
        uisa_dec_t a;
        a = uisa_attrs(d.op);
        a.rd = d.rd;
        a.rs1 = d.rs1;
        a.rs2 = d.rs2;
        a.imm = d.imm;
        a.tie_violation = d.tie_violation;
        if (a != d) begin
          $display("uisa_attrs(%0d) differs from the decode of %08x: %h vs %h", d.op, words[i], a, d);
          fails++;
        end
      end
    end
    if (uisa_attrs(UISA_ILLEGAL).valid) fails++;
    if (uisa_decode(32'hc4000000).op != UISA_ILLEGAL) fails++;  // major 49: reserved for FP
    // the millicode predicate u: reserved um / lk / sz values, and the u bits
    if (uisa_decode({6'd36, 12'd0, 2'd1, 2'd3, 10'd0}).op != UISA_ILLEGAL) fails++;   // probe.r um=3
    if (uisa_decode({6'd37, 12'd0, 2'd2, 2'd3, 10'd0}).op != UISA_ILLEGAL) fails++;   // probe.w um=3
    if (uisa_decode({6'd54, 12'd0, 2'd1, 3'd5, 9'd0}).op != UISA_ILLEGAL) fails++;   // ld.c lk=5
    if (uisa_decode({6'd54, 12'd0, 2'd1, 3'd7, 9'd0}).op != UISA_ILLEGAL) fails++;   // ld.c lk=7
    if (uisa_decode({6'd53, 12'd0, 2'd1, 2'd3, 1'd1, 9'd0}).op != UISA_ILLEGAL) fails++;   // st.c sz=3
    begin
      uisa_dec_t p, a, l, s, b, e;
      p = uisa_decode({6'd36, 6'd5, 6'd10, 2'd1, 2'd1, 10'd0});        // probe.r.us.gd
      a = uisa_decode({6'd37, 6'd5, 6'd10, 2'd1, 2'd2, 10'd0});        // probe.w.ua.gd
      l = uisa_decode({6'd54, 6'd11, 6'd10, 2'd1, 3'd4, 9'd0});        // ld.c.32.gd
      s = uisa_decode({6'd53, 6'd13, 6'd10, 2'd1, 2'd2, 1'd1, 9'd0});  // stu.c.32.gd
      b = uisa_decode({6'd16, 12'd0, 3'd7, 11'd2});                     // b.nu
      e = uisa_decode({6'd16, 6'd1, 6'd2, 3'd0, 11'd2});                // b.eq
      if (p.op != UISA_PROBE_R_US_GD || !p.writes_u || p.reads_u || !p.is_probe) fails++;
      if (a.op != UISA_PROBE_W_UA_GD || !a.writes_u || !a.reads_u) fails++;
      if (l.op != UISA_LD_C_32_GD || !l.reads_u || !l.is_load || l.mem_bytes != 3'd4) fails++;
      if (s.op != UISA_STU_C_32_GD || !s.reads_u || !s.is_store || !s.lock_end || !s.reads_rd) fails++;
      if (b.op != UISA_B_NU || !b.reads_u || b.reads_rs1 || b.imm != 32'd8) fails++;
      if (e.op != UISA_B_EQ || e.reads_u) fails++;
    end
    $display("tb_decode: %0d vectors, %0d failures", count, fails);
    if (fails != 0) $fatal(1);
    $finish;
  end
endmodule
