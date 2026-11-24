struct pt { s32 x; s16 y; u8 tag; u64 big; struct pt *next; };
u32 g_a[10] = {1, 2, 3, 4, 5};
s16 g_m[3][4] = {{1, -2, 3, 4}, {5, 6, -7, 8}, {9, 10, 11, -12}};
struct pt g_pts[4] = {{1, 2, 3, 4, 0}, {-5, -6, 7, 0x123456789ull, 0}};
u32 *g_p = &g_a[2];
s16 *g_q = &g_m[1][1];
u64 g_w = 0xfedcba9876543210ull;
u8 g_bytes[7];
s32 g_count;

u32 crc;
void mix(u32 v) {
  u32 k;
  crc ^= v;
  for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
}
void swap(u32 *a, u32 *b) { u32 t = *a; *a = *b; *b = t; }
s32 sum(s32 *v, s32 n) {
  s32 s = 0;
  for (s32 i = 0; i < n; i++) s += v[i];
  return s;
}
void bump(struct pt *p, s32 d) {
  p->x += d;
  p->y = p->y * 3 - d;
  p->tag++;
  p->big = p->big * 7 + (u64)d;
}
u32 rec(u32 *arr, u32 n) {   // recursion with a local array
  u32 local[4];
  if (n == 0) return 1;
  for (u32 i = 0; i < 4; i++) local[i] = arr[i] + n;
  return local[n & 3] + rec(local, n - 1) * 3;
}
int main(void) {
  u32 i, x = 5, y = 9;
  s32 loc[6] = {3, -1, 4, -1, 5};
  struct pt s = {7, 8, 9, 10, 0};
  struct pt *pp = &s;
  swap(&x, &y);
  mix(x); mix(y);
  for (i = 0; i < 10; i++) g_a[i] += i * i;
  swap(g_p, g_p + 3);
  for (i = 0; i < 10; i++) mix(g_a[i]);
  *g_q += 100;
  for (i = 0; i < 3; i++)
    for (u32 j = 0; j < 4; j++) mix((u32)g_m[i][j]);
  mix((u32)sum(loc, 6));
  bump(pp, 3);
  bump(&g_pts[1], -2);
  g_pts[2] = g_pts[1];
  g_pts[2].next = &g_pts[0];
  g_pts[2].next->x = 77;
  mix((u32)s.x); mix((u32)s.y); mix(s.tag); mix((u32)s.big); mix((u32)(s.big >> 32));
  for (i = 0; i < 4; i++) {
    mix((u32)g_pts[i].x); mix((u32)g_pts[i].y); mix(g_pts[i].tag);
    mix((u32)g_pts[i].big); mix((u32)(g_pts[i].big >> 32));
  }
  mix((u32)g_w); mix((u32)(g_w >> 32));
  g_w = g_w * 3 + 1;
  mix((u32)(g_w >> 32));
  for (i = 0; i < 7; i++) g_bytes[i] = (u8)(i * 37 + 200);
  for (i = 0; i < 7; i++) mix(g_bytes[i]);
  mix(rec(g_a, 6));
  u32 *q = &g_a[9];
  mix((u32)(q - g_p));
  mix(*(q - 1) + (q > g_p) + (q == &g_a[9]));
  g_count = (s32)(i, x + y);
  mix((u32)g_count);
  return crc;
}
