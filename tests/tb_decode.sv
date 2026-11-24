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
    end
    if (uisa_decode(32'hc4000000).op != UISA_ILLEGAL) fails++;  // major 49: reserved for FP
    $display("tb_decode: %0d vectors, %0d failures", count, fails);
    if (fails != 0) $fatal(1);
    $finish;
  end
endmodule
