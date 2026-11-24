// Array fields in structs, struct values that carry arrays, and sizeof.
// sizeof is only applied to types whose layout is the same on the LP64
// host (no pointers, no 64-bit fields inside structs).
struct vec { s32 n; s16 v[5]; u8 tag; };
struct grid { u8 cells[3][4]; struct vec rows[2]; u32 sum; };
struct big { u32 w[2 * 5 + 1]; };

struct vec g_v = {3, {10, -20, 30}, 7};
struct grid g_g = {{{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}}, {{1, {1, 2}, 3}, {2, {-4, 5, 6}, 9}}, 77};
struct vec g_vs[3];

u32 crc;
void mix(u32 v) {
  u32 k;
  crc ^= v;
  for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
}
void mixvec(struct vec v) {   // by value: the callee copies it to its frame
  mix((u32)v.n);
  for (s32 i = 0; i < 5; i++) mix((u32)v.v[i]);
  mix(v.tag);
}
struct vec scale(struct vec v, s16 k) {   // returned through the hidden pointer
  for (s32 i = 0; i < 5; i++) v.v[i] = (s16)(v.v[i] * k);
  v.n++;
  return v;
}
struct big fill(u32 seed) {
  struct big b;
  for (u32 i = 0; i < sizeof(b.w) / sizeof(b.w[0]); i++) b.w[i] = seed * (i + 1) ^ (seed >> i);
  return b;
}
s32 total(struct vec *p) {
  s32 t = 0;
  s16 *q = p->v;   // the array field decays
  for (s32 i = 0; i < 5; i++) t += q[i];
  return t + p->n;
}
s16 third(void) { return scale(g_v, 2).v[2]; }   // a field of a returned value

int main(void) {
  struct vec a = g_v;
  struct vec b;
  struct grid g;
  s32 i, j;
  a.v[4] = 99;
  b = a;   // whole-struct copy, arrays included
  b.v[0] = -1;
  mixvec(a);
  mixvec(b);
  mixvec(scale(b, -3));
  mix((u32)total(&a));
  mix((u32)total(&g_v));
  mix((u32)third());
  g = g_g;
  g.rows[1].v[2] += g.cells[2][3];
  g_g.cells[0][0] = 200;
  for (i = 0; i < 3; i++)
    for (j = 0; j < 4; j++) mix(g.cells[i][j] + g_g.cells[i][j]);
  for (i = 0; i < 2; i++) mixvec(g.rows[i]);
  mix(g.sum);
  for (i = 0; i < 3; i++) {
    g_vs[i] = scale(a, (s16)(i + 1));
    g_vs[i].v[i] ^= 0x55;
    mixvec(g_vs[i]);
  }
  struct big bb = fill(0x1234567u);
  for (i = 0; i < 2 * 5 + 1; i++) mix(bb.w[i]);
  mix(fill(3).w[7]);
  // sizeof of types, objects and expressions (none are evaluated)
  mix((u32)sizeof(struct vec));
  mix((u32)sizeof(struct grid));
  mix((u32)sizeof g.cells);
  mix((u32)sizeof g.cells[1]);
  mix((u32)sizeof(g_vs));
  mix((u32)sizeof(g_vs) / (u32)sizeof g_vs[0]);
  mix((u32)sizeof(s16[3][5]));
  mix((u32)sizeof(u8));
  mix((u32)sizeof(s64));
  mix((u32)sizeof(long));
  mix((u32)sizeof(i++));
  mix((u32)sizeof(total(&a)));
  mix((u32)(sizeof(u16) + sizeof(-a.v[1]) * 3));
  mix((u32)i);
  return (int)crc;
}
