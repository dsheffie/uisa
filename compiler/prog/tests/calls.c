u32 fib(u32 n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
u64 many(u32 a, u32 b, u32 c, u32 d, u32 e, u64 f, u32 g, u32 h, u32 i, u32 j) {
  return (u64)a * 3 + b + c * 5 + d + e * 7 + f + g + h * 11 + i + j * 13;
}
s32 ack(s32 m, s32 n) {
  if (m == 0) return n + 1;
  if (n == 0) return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}
int main(void) {
  u32 s = fib(20);
  u64 w = many(1, 2, 3, 4, 5, 0x100000000ull, 7, 8, 9, 10);
  s = s * 31 + (u32)w + (u32)(w >> 32);
  s = s * 31 + ack(2, 3);
  return s;
}
