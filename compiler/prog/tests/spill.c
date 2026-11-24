// Register pressure across calls: more values live than registers, so the
// allocator spills; values live across calls need callee-saved registers.
u32 h(u32 x) { return x * 2654435761u + 1; }
u32 spill(u32 seed) {
  u32 v0 = h(seed), v1 = h(v0), v2 = h(v1), v3 = h(v2), v4 = h(v3), v5 = h(v4), v6 = h(v5), v7 = h(v6);
  u32 v8 = h(v7), v9 = h(v8), v10 = h(v9), v11 = h(v10), v12 = h(v11), v13 = h(v12), v14 = h(v13);
  u32 v15 = h(v14), v16 = h(v15), v17 = h(v16), v18 = h(v17), v19 = h(v18), v20 = h(v19), v21 = h(v20);
  u32 v22 = h(v21), v23 = h(v22), v24 = h(v23), v25 = h(v24), v26 = h(v25), v27 = h(v26), v28 = h(v27);
  u32 v29 = h(v28), v30 = h(v29), v31 = h(v30), v32 = h(v31), v33 = h(v32), v34 = h(v33), v35 = h(v34);
  u32 s = 0;
  for (u32 i = 0; i < 3; i++) {
    s = s * 33 + (v0 ^ v35) + (v1 ^ v34) + (v2 ^ v33) + (v3 ^ v32) + (v4 ^ v31) + (v5 ^ v30) + (v6 ^ v29);
    s = s * 33 + (v7 ^ v28) + (v8 ^ v27) + (v9 ^ v26) + (v10 ^ v25) + (v11 ^ v24) + (v12 ^ v23);
    s = s * 33 + (v13 ^ v22) + (v14 ^ v21) + (v15 ^ v20) + (v16 ^ v19) + (v17 ^ v18) + h(s);
    v0 += s; v7 ^= s; v14 -= s; v21 += v0; v28 ^= v7; v35 += h(v14);
  }
  return s ^ v0 ^ v7 ^ v14 ^ v21 ^ v28 ^ v35;
}
// 64-bit values: twice the words
u64 spill64(u64 a) {
  u64 w0 = a * 3, w1 = w0 ^ 0x123456789abcdefull, w2 = w1 + w0, w3 = w2 * w1, w4 = w3 - w2, w5 = w4 ^ (w3 >> 7);
  u64 w6 = w5 + w4 * 5, w7 = w6 ^ (w5 << 9), w8 = w7 + w6, w9 = w8 * 0x9e3779b97f4a7c15ull, w10 = w9 ^ w8;
  u64 w11 = w10 + w9, w12 = w11 * w10, w13 = w12 ^ (w11 >> 13), w14 = w13 + w12, w15 = w14 ^ w13;
  u64 w16 = w15 * 7 + (u64)h((u32)w15);
  return w0 + w1 + w2 + w3 + w4 + w5 + w6 + w7 + w8 + w9 + w10 + w11 + w12 + w13 + w14 + w15 + w16;
}
int main(void) {
  u64 r = spill64(0xdeadbeefcafef00dull);
  return spill(12345) ^ (u32)r ^ (u32)(r >> 32);
}
