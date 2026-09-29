/**
 ****************************************************************************************************
 * @file        ntc.c
 * @brief       NTC 10K B3950 查表换算（表 = 离线按 B 参数式生成，见下）
 * @note        ★ 表生成式（可复现）：
 *                R(code) = 10000 * code / (4095 - code)      Ω
 *                T(R)    = 1/(1/298.15 + ln(R/10000)/3950) - 273.15   ℃
 *              每 16 个 code 一项（256 项），拖尾拓展到 [-40.0, +125.0]℃（超出即销位）；
 *              项间**线性插值**（温度对 code 单调递减）。
 ****************************************************************************************************
 */

#include "./BSP/NTC/ntc.h"

#define NTC_TAB_STEP    16u                 /* 每多少个 code 一项 */
#define NTC_TAB_N       256u                /* 256 项 × int16 = 512 B */

/* ★ 离线生成（单位 0.1℃）：下标 i 对应 code = i*16 */
static const int16_t s_tab[NTC_TAB_N] =
{
      1250,   1250,   1250,   1250,   1250,   1250,   1250,   1250,
      1250,   1244,   1200,   1162,   1127,   1096,   1067,   1041,
      1016,    993,    972,    951,    933,    915,    898,    881,
       866,    851,    837,    824,    811,    798,    786,    774,
       763,    752,    742,    732,    722,    712,    703,    694,
       685,    676,    668,    659,    651,    644,    636,    628,
       621,    614,    607,    600,    593,    586,    580,    573,
       567,    561,    554,    548,    542,    537,    531,    525,
       520,    514,    509,    503,    498,    493,    487,    482,
       477,    472,    467,    462,    457,    453,    448,    443,
       439,    434,    429,    425,    420,    416,    412,    407,
       403,    399,    394,    390,    386,    382,    378,    374,
       369,    365,    361,    357,    353,    350,    346,    342,
       338,    334,    330,    326,    323,    319,    315,    311,
       308,    304,    300,    296,    293,    289,    286,    282,
       278,    275,    271,    268,    264,    260,    257,    253,
       250,    246,    243,    239,    236,    232,    229,    225,
       222,    219,    215,    212,    208,    205,    201,    198,
       194,    191,    187,    184,    181,    177,    174,    170,
       167,    163,    160,    156,    153,    150,    146,    143,
       139,    136,    132,    129,    125,    122,    118,    115,
       111,    108,    104,    100,     97,     93,     90,     86,
        82,     79,     75,     71,     68,     64,     60,     56,
        53,     49,     45,     41,     37,     33,     29,     25,
        22,     17,     13,      9,      5,      1,     -3,     -7,
       -12,    -16,    -20,    -25,    -29,    -34,    -38,    -43,
       -47,    -52,    -57,    -62,    -67,    -72,    -77,    -82,
       -87,    -92,    -98,   -103,   -109,   -114,   -120,   -126,
      -132,   -138,   -145,   -151,   -158,   -165,   -172,   -179,
      -186,   -194,   -202,   -210,   -219,   -228,   -237,   -246,
      -257,   -267,   -278,   -290,   -303,   -317,   -331,   -347,
      -365,   -384,   -400,   -400,   -400,   -400,   -400,   -400,
};

static uint16_t s_selftest_rc = 0xFFFFu;

/* ★★ 2026-09-29：自检的**入参必须是编译器折不掉的**。
 *   缘由：上一版用字面常数调用，编译器把换算函数与**整张表**全删
 *   （map：`Removing ntc.o(.rodata.s_tab), (512 bytes)`）⇒ 自检退化成一个**编译期常量**。
 *   那不叫自检：它没有验到任何**运行期路径**。★ 同理适用于本工程其他注入式自检。 */
static volatile const uint16_t s_case_code[4] = { 2047u, 1081u, 3156u, 3800u };

uint8_t ntc_code_to_temp_x10(uint16_t code, int16_t *t_x10)
{
    uint16_t i;
    int32_t  v0;
    int32_t  v1;
    int32_t  v;

    if (t_x10 == NULL) { return NTC_ERR_RANGE; }

    if (code > (uint16_t)NTC_CODE_MAX) { return NTC_ERR_RANGE; }

    i = (uint16_t)(code / NTC_TAB_STEP);

    if (i >= (NTC_TAB_N - 1u))
    {
        v = (int32_t)s_tab[NTC_TAB_N - 1u];
    }
    else
    {
        v0 = (int32_t)s_tab[i];
        v1 = (int32_t)s_tab[i + 1u];
        v = v0 + (((v1 - v0) * (int32_t)(code % NTC_TAB_STEP)) / (int32_t)NTC_TAB_STEP);
    }

    /* ★ 量程判定：落在钳位边界上 = 本来就超出 ±量程 ⇒ 回错代码，⛔ 不当读数 */
    if ((v <= -400) || (v >= 1250)) { return NTC_ERR_RANGE; }

    *t_x10 = (int16_t)v;

    return NTC_OK;
}

uint16_t ntc_selftest_result(void)
{
    return s_selftest_rc;
}

/**
 * @brief       离线自检：用离线算出的真值核对换算
 * @note        ★ 第 1 条是**拓扑判决点**：25℃ 时 R_ntc == R_pull ⇒ code ≈ 2047。
 *              接反了（NTC 在上拉位）会得约 55℃ ⇒ 本条立刻失败。
 */
uint16_t ntc_selftest(void)
{
    int16_t t;

    if (ntc_code_to_temp_x10(s_case_code[0], &t) != NTC_OK) { s_selftest_rc = 1u; return 1u; }
    if ((t < 240) || (t > 260))                      { s_selftest_rc = 1u; return 1u; }

    if (ntc_code_to_temp_x10(s_case_code[1], &t) != NTC_OK) { s_selftest_rc = 2u; return 2u; }
    if ((t < 480) || (t > 520))                      { s_selftest_rc = 2u; return 2u; }

    if (ntc_code_to_temp_x10(s_case_code[2], &t) != NTC_OK) { s_selftest_rc = 3u; return 3u; }
    if ((t < -30) || (t > 30))                       { s_selftest_rc = 3u; return 3u; }

    if (ntc_code_to_temp_x10(s_case_code[3], &t) != NTC_OK) { s_selftest_rc = 4u; return 4u; }
    if ((t < -250) || (t > -200))                    { s_selftest_rc = 4u; return 4u; }

    if (ntc_code_to_temp_x10(0u, &t) != NTC_ERR_RANGE)      { s_selftest_rc = 5u; return 5u; }
    if (ntc_code_to_temp_x10(4095u, &t) != NTC_ERR_RANGE)   { s_selftest_rc = 5u; return 5u; }
    if (ntc_code_to_temp_x10(5000u, &t) != NTC_ERR_RANGE)   { s_selftest_rc = 5u; return 5u; }

    {
        int16_t t1, t2, t3;
        if (ntc_code_to_temp_x10((uint16_t)(s_case_code[1] - 847u), &t1) != NTC_OK) { s_selftest_rc = 6u; return 6u; }
        if (ntc_code_to_temp_x10(s_case_code[0], &t2) != NTC_OK) { s_selftest_rc = 6u; return 6u; }
        if (ntc_code_to_temp_x10((uint16_t)(s_case_code[2] + 44u), &t3) != NTC_OK) { s_selftest_rc = 6u; return 6u; }
        if (!((t1 > t2) && (t2 > t3))) { s_selftest_rc = 6u; return 6u; }
    }

    s_selftest_rc = 0u;
    return 0u;
}
