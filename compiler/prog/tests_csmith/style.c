/*
 * Written by hand in the shape of csmith 2.3 output (csmith is not installed
 * here): exercises compiler/prog/csmith/csmith.h and the constructs csmith
 * emits (static/const, long constants, safe math, struct returns by value,
 * pointers to pointers, labels, main(argc, argv)).
 */
#include "csmith.h"

static long __undefined;

/* --- Struct/Union Declarations --- */
struct S0 {
   int32_t  f0;
   uint16_t  f1;
   int64_t  f2;
   uint8_t  f3;
   int8_t  f4;
};

/* --- GLOBAL VARIABLES --- */
static int32_t g_2 = 0x4C5B0E2AL;
static int32_t *g_5 = &g_2;
static int32_t ** const g_6 = &g_5;
static uint8_t g_13[3][2] = {{0xC2L,0x6AL},{0xC2L,0x6AL},{0xC2L,0x6AL}};
static struct S0 g_30 = {-1L,0x5C7BL,0x1B8A3C3D5E6F7A8BLL,255UL,(-7L)};
static struct S0 g_31[2] = {{0L,0UL,-1L,1UL,1L},{0x7FFFFFFFL,65535UL,(-9223372036854775807L - 1),0UL,(-128L)}};
static uint64_t g_44 = 18446744073709551615UL;
static int16_t g_50 = (-1L);
static const uint32_t g_60 = 4294967295UL;
static unsigned long g_70 = 3UL;
static long long g_71 = -5LL;
static signed char g_72 = -3;
static unsigned short g_73 = 0xFFFEU;

/* --- FORWARD DECLARATIONS --- */
static int32_t  func_1(void);
static struct S0  func_7(int32_t * p_8, uint32_t  p_9);
static uint8_t  func_15(int64_t  p_16, struct S0  p_17, int32_t ** p_18);

/* --- FUNCTIONS --- */
/* ------------------------------------------ */
/*
 * reads : g_2 g_5 g_13 g_30
 * writes: g_2 g_13 g_30
 */
static int32_t  func_1(void)
{ /* block id: 0 */
    int32_t *l_3 = &g_2;
    int32_t l_4[2];
    struct S0 l_20 = {0x3A2BL,0x1234L,-1L,9UL,-2L};
    int32_t **l_21 = &l_3;
    int i;
    for (i = 0; i < 2; i++)
        l_4[i] = 0L;
    (*l_3) = safe_add_func_int32_t_s_s((*l_3), l_4[1]);
    for (g_50 = 0; (g_50 <= 1); g_50 += 1)
    { /* block id: 3 */
        uint32_t l_10 = 0xD3A1L;
        g_30 = func_7(&l_4[g_50], l_10);
        (**l_21) ^= safe_mul_func_int16_t_s_s(g_50, (int16_t)g_30.f1);
        if (g_30.f0)
            break;
        g_13[(g_50 + 1)][g_50] = func_15(g_30.f2, l_20, l_21);
    }
    l_20.f2 = safe_sub_func_int64_t_s_s(l_20.f2, (((*l_3) , g_44) != g_60));
    g_31[1] = l_20;
lbl_40:
    (*g_5) = safe_lshift_func_uint8_t_u_s(g_13[2][1], 3);
    if (((*l_3) = safe_mod_func_int32_t_s_s((**g_6), (int32_t)g_70)) > 100L)
        goto lbl_40;
    g_71 = safe_div_func_int64_t_s_s(g_71, g_72) + (long)__undefined;
    g_73 = safe_rshift_func_uint16_t_u_u(g_73, (uint32_t)g_72);
    return (*l_3);
}

/* ------------------------------------------ */
static struct S0  func_7(int32_t * p_8, uint32_t  p_9)
{ /* block id: 4 */
    struct S0 l_11 = {0L,0UL,0L,0UL,0L};
    (*p_8) = safe_unary_minus_func_int32_t_s(safe_div_func_int32_t_s_s(p_9, (*g_5)));
    l_11.f0 = (*p_8);
    l_11.f1 = safe_add_func_uint16_t_u_u(g_30.f1, p_9);
    l_11.f2 = safe_mul_func_int64_t_s_s(g_30.f2, (int64_t)p_9);
    l_11.f3 = safe_lshift_func_uint8_t_u_u(g_30.f3, 1);
    l_11.f4 = safe_rshift_func_int8_t_s_s(g_30.f4, 1);
    return l_11;
}

/* ------------------------------------------ */
static uint8_t  func_15(int64_t  p_16, struct S0  p_17, int32_t ** p_18)
{ /* block id: 8 */
    uint16_t l_19[3][3] = {{1UL,0xA7E9L,1UL},{1UL,0xA7E9L,1UL},{1UL,0xA7E9L,1UL}};
    int i, j;
    (**p_18) |= safe_mod_func_uint16_t_u_u(l_19[2][1], p_17.f1);
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            p_16 = safe_add_func_int64_t_s_s(p_16, l_19[i][j]);
    return (uint8_t)(p_16 ^ p_17.f3);
}

/* ---------------------------------------- */
int main (int argc, char* argv[])
{
    int i, j;
    int print_hash_value = 0;
    if (argc == 2 && strcmp(argv[1], "1") == 0) print_hash_value = 1;
    platform_main_begin();
    crc32_gentab();
    func_1();
    transparent_crc(g_2, "g_2", print_hash_value);
    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 2; j++)
        {
            transparent_crc(g_13[i][j], "g_13[i][j]", print_hash_value);
            if (print_hash_value) printf("index = [%d][%d]\n", i, j);
        }
    }
    transparent_crc(g_30.f0, "g_30.f0", print_hash_value);
    transparent_crc(g_30.f1, "g_30.f1", print_hash_value);
    transparent_crc(g_30.f2, "g_30.f2", print_hash_value);
    transparent_crc(g_30.f3, "g_30.f3", print_hash_value);
    transparent_crc(g_30.f4, "g_30.f4", print_hash_value);
    for (i = 0; i < 2; i++)
    {
        transparent_crc(g_31[i].f0, "g_31[i].f0", print_hash_value);
        transparent_crc(g_31[i].f2, "g_31[i].f2", print_hash_value);
        transparent_crc(g_31[i].f4, "g_31[i].f4", print_hash_value);
    }
    transparent_crc(g_44, "g_44", print_hash_value);
    transparent_crc(g_50, "g_50", print_hash_value);
    transparent_crc(g_70, "g_70", print_hash_value);
    transparent_crc(g_71, "g_71", print_hash_value);
    transparent_crc(g_72, "g_72", print_hash_value);
    transparent_crc(g_73, "g_73", print_hash_value);
    platform_main_end(crc32_context ^ 0xFFFFFFFFUL, print_hash_value);
    return 0;
}
