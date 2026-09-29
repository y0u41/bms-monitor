/**
 ****************************************************************************************************
 * @file        app_calib.c
 * @brief       标定表区的存储层（规格 = `Tech_Design.md` §8「标定表区协议」／ `附录C` C-127）
 * @note        ★ 纯整数、⛔ 不碰外设、⛔ 不碰告警/执行器/通信。
 *              ★ **记录**（页头 6 半字 ＋ 载荷 133 半字 = 139 半字 = 278 B）是编解码的**单位**，
 *                写盘时把它放进整页镜像的对应偏移 —— 这样"整页 RMW"不会抹掉另一份副本（见 `app_calib_poll()`）。
 ****************************************************************************************************
 */

#include "./App/app_calib.h"
#include "./App/app_log.h"              /* ★ 复用 CCITT-FALSE 的唯一实现 */
#include "./BSP/STMFLASH/stmflash.h"
#include <string.h>

/* ===================== 记录布局（★ 与 §8 协议 (1)(2) 逐项对应） ===================== */
#define CAL_HDR_HW      6u                              /* 魔数/版本/序号(2)/CRC/长度 */
#define CAL_PAY_HW      (1u + 2u + 1u + (APP_CALIB_CELL_MAX * 2u) + (APP_CALIB_CELL_MAX * 2u) + 1u + (APP_CALIB_OCV_MAX * 2u))
#define CAL_REC_HW      (CAL_HDR_HW + CAL_PAY_HW)       /* 139 半字 */

#define CAL_MAGIC       0u
#define CAL_VER         1u
#define CAL_SEQ_LO      2u
#define CAL_SEQ_HI      3u
#define CAL_CRC         4u
#define CAL_LEN         5u

/* 载荷内偏移（半字） */
#define CAL_P_OFS       CAL_HDR_HW      /* ★★ 载荷从**页头之后**开始！写成 0 会让载荷
                                        *   盖掉魔数/序号等判据字段（本轮自检就是这么抓出来的） */
#define CAL_P_CELLS     (CAL_P_OFS + 0u)
#define CAL_P_UNIX      (CAL_P_OFS + 1u)
#define CAL_P_T         (CAL_P_OFS + 3u)
#define CAL_P_K         (CAL_P_OFS + 4u)
#define CAL_P_B         (CAL_P_K + (APP_CALIB_CELL_MAX * 2u))
#define CAL_P_OCVN      (CAL_P_B + (APP_CALIB_CELL_MAX * 2u))
#define CAL_P_OCV       (CAL_P_OCVN + 1u)

/* 判定状态 */
#define CAL_ST_BLANK    0u
#define CAL_ST_OK       1u
#define CAL_ST_OLDVER   2u
#define CAL_ST_CORRUPT  3u
#define CAL_ST_BADCELLS 4u      /* 串数与 `APP_CELL_COUNT` 不符 ⇒ 整表拒用（⛔ 不是"坏"，是"不适用"） */

/* ===================== 模块状态 ===================== */
static uint32_t s_k[APP_CALIB_CELL_MAX];   /* ★ k_ppm 是 u32（ppm 口径，1000000 = 不修正）—— ⛔ 不能存成 u16 */
static int32_t  s_b[APP_CALIB_CELL_MAX];
static uint16_t s_ocv_soc[APP_CALIB_OCV_MAX];
static uint16_t s_ocv_mv[APP_CALIB_OCV_MAX];

static uint8_t  s_valid;            /* 1 = 有一份可信表（本版预期 0） */
static uint8_t  s_cells;
static uint8_t  s_ocv_n;
static uint8_t  s_active;           /* 活动副本 0/1 */
static uint32_t s_seq;
static uint32_t s_unix;
static int16_t  s_t_x10;
static uint8_t  s_pending;
static uint8_t  s_last_rc;
static uint8_t  s_dbg[4];           /* 取证：[0]=副本0 状态 [1]=副本1 状态 [2]=用的哪份 [3]=最近一次落盘 rc */

static uint16_t s_page[APP_FLASH_PAGE_SIZE / 2u];   /* 2 KB 整页镜像（唯一的大缓冲） */
static uint16_t s_selftest_rc = 0xFFFFu;

static uint32_t calib_copy_off(uint8_t copy)
{
    return (copy == 0u) ? 0u : APP_CALIB_COPY_HW;
}

/* ===================== 编解码（★ 纯缓冲 ⇒ 可离线自检，⛔ 不碰 Flash） ===================== */

/** ★ CRC 覆盖"除 CRC 半字本身之外的全部页头 ＋ 载荷" —— 与参数区**同口径** */
static uint16_t calib_crc(uint16_t *rec)
{
    uint16_t saved = rec[CAL_CRC];
    uint16_t c;

    rec[CAL_CRC] = 0x0000u;
    c = app_log_crc16_hw(rec, (uint16_t)CAL_REC_HW);
    rec[CAL_CRC] = saved;

    return c;
}

/** 把 RAM 里的标定表打包成一条记录（含页头与 CRC） */
static void calib_pack(uint16_t *rec, uint32_t seq, uint8_t cells, uint32_t unix_s, int16_t t_x10)
{
    uint16_t i;

    memset(rec, 0xFF, (size_t)(CAL_REC_HW * 2u));

    rec[CAL_MAGIC]  = APP_CALIB_MAGIC;
    rec[CAL_VER]    = APP_CALIB_VER;
    rec[CAL_SEQ_LO] = (uint16_t)(seq & 0xFFFFu);
    rec[CAL_SEQ_HI] = (uint16_t)(seq >> 16);
    rec[CAL_CRC]    = 0x0000u;
    rec[CAL_LEN]    = (uint16_t)CAL_PAY_HW;

    rec[CAL_P_CELLS] = cells;
    rec[CAL_P_UNIX]  = (uint16_t)(unix_s & 0xFFFFu);
    rec[CAL_P_UNIX + 1u] = (uint16_t)(unix_s >> 16);
    rec[CAL_P_T]     = (uint16_t)t_x10;

    for (i = 0u; i < (uint16_t)APP_CALIB_CELL_MAX; i++)
    {
        rec[CAL_P_K + (i * 2u)]      = (uint16_t)(s_k[i] & 0xFFFFu);
        rec[CAL_P_K + (i * 2u) + 1u] = (uint16_t)(s_k[i] >> 16);

        rec[CAL_P_B + (i * 2u)]      = (uint16_t)((uint32_t)s_b[i] & 0xFFFFu);
        rec[CAL_P_B + (i * 2u) + 1u] = (uint16_t)((uint32_t)s_b[i] >> 16);
    }

    rec[CAL_P_OCVN] = s_ocv_n;

    for (i = 0u; i < (uint16_t)APP_CALIB_OCV_MAX; i++)
    {
        rec[CAL_P_OCV + (i * 2u)]      = s_ocv_soc[i];
        rec[CAL_P_OCV + (i * 2u) + 1u] = s_ocv_mv[i];
    }

    rec[CAL_CRC] = calib_crc(rec);
}

/**
 * @brief       解析一条记录
 * @param[out]  info  仅在返回 `CAL_ST_OK` 时被填充
 * @retval      `CAL_ST_*`
 * @note        ★ 四类必须分开：空白（没写过）/ 可用 / 版本是别的（⇒ 迁移）/ 内容不可信；
 *              另有"不适用"（串数不符）。⛔ 把"版本旧"当"坏"会让一次升级把标定判死。
 */
static uint8_t calib_parse(const uint16_t *rec, app_calib_info_t *info)
{
    uint16_t len;
    uint16_t crc;
    uint32_t seq;

    if (rec[CAL_MAGIC] != APP_CALIB_MAGIC)
    {
        return CAL_ST_BLANK;                    /* 空白页 0xFFFF ⇒ 从未写过（⛔ 不是"坏"） */
    }

    len = rec[CAL_LEN];

    if (len != (uint16_t)CAL_PAY_HW)
    {
        return CAL_ST_CORRUPT;
    }

    {
        uint16_t tmp[CAL_REC_HW];

        memcpy(tmp, rec, sizeof(tmp));
        crc = calib_crc(tmp);
    }

    if (crc != rec[CAL_CRC])
    {
        return CAL_ST_CORRUPT;                  /* ★ 判据字段（序号/版本/长度）受 CRC 保护 */
    }

    if (rec[CAL_VER] != APP_CALIB_VER)
    {
        return CAL_ST_OLDVER;                   /* 可读但版本不同 ⇒ 迁移，⛔ 不锁定 */
    }

    if (rec[CAL_P_CELLS] != (uint16_t)APP_CELL_COUNT)
    {
        return CAL_ST_BADCELLS;                 /* ★ 换了串数 ⇒ 旧标定在语义上就是错的 */
    }

    if (info != NULL)
    {
        uint16_t i;

        seq = (uint32_t)rec[CAL_SEQ_LO] | ((uint32_t)rec[CAL_SEQ_HI] << 16);

        info->valid      = 1u;
        info->cells      = (uint8_t)rec[CAL_P_CELLS];
        info->ocv_n      = (uint8_t)rec[CAL_P_OCVN];
        info->seq        = seq;
        info->calib_unix = (uint32_t)rec[CAL_P_UNIX] | ((uint32_t)rec[CAL_P_UNIX + 1u] << 16);
        info->t_calib_x10 = (int16_t)rec[CAL_P_T];

        for (i = 0u; i < (uint16_t)APP_CALIB_CELL_MAX; i++)
        {
            s_k[i] = (uint16_t)rec[CAL_P_K + (i * 2u)] |
                     ((uint16_t)rec[CAL_P_K + (i * 2u) + 1u] << 16);
            s_b[i] = (int32_t)((uint32_t)rec[CAL_P_B + (i * 2u)] |
                               ((uint32_t)rec[CAL_P_B + (i * 2u) + 1u] << 16));
        }

        if (info->ocv_n > (uint8_t)APP_CALIB_OCV_MAX) { return CAL_ST_CORRUPT; }

        s_ocv_n = info->ocv_n;

        for (i = 0u; i < (uint16_t)APP_CALIB_OCV_MAX; i++)
        {
            s_ocv_soc[i] = rec[CAL_P_OCV + (i * 2u)];
            s_ocv_mv[i]  = rec[CAL_P_OCV + (i * 2u) + 1u];
        }
    }

    return CAL_ST_OK;
}

/* ===================== 上电装载 ===================== */

void app_calib_init(void)
{
    uint16_t  rec[CAL_REC_HW];
    uint8_t   st[2];
    app_calib_info_t info[2];
    uint8_t   c;

    s_valid = 0u;
    s_pending = 0u;
    s_active = 0u;
    s_seq = 0u;
    s_cells = 0u;
    s_ocv_n = 0u;
    s_unix = 0u;
    s_t_x10 = 0;
    s_last_rc = 0xFFu;
    memset(s_k, 0, sizeof(s_k));
    memset(s_b, 0, sizeof(s_b));
    memset(s_ocv_soc, 0, sizeof(s_ocv_soc));
    memset(s_ocv_mv, 0, sizeof(s_ocv_mv));
    memset(info, 0, sizeof(info));

    for (c = 0u; c < 2u; c++)
    {
        stmflash_read(APP_CALIB_ADDR + (calib_copy_off(c) * 2u), rec, (uint16_t)CAL_REC_HW);
        st[c] = calib_parse(rec, &info[c]);
    }

    s_dbg[0] = st[0];
    s_dbg[1] = st[1];

    /* ★ 选法：两份都可用取序号大者；只有一份可用用它；都不可用 ⇒ valid = 0（**正常回退**，⛔ 不拒绝启动） */
    if ((st[0] == CAL_ST_OK) && (st[1] == CAL_ST_OK))
    {
        c = (info[1].seq > info[0].seq) ? 1u : 0u;
        stmflash_read(APP_CALIB_ADDR + (calib_copy_off(c) * 2u), rec, (uint16_t)CAL_REC_HW);
        (void)calib_parse(rec, &info[c]);
        s_active = c;
    }
    else if (st[0] == CAL_ST_OK) { s_active = 0u; }
    else if (st[1] == CAL_ST_OK) { s_active = 1u; }
    else
    {
        s_valid = 0u;
        s_dbg[2] = 0xFFu;
        return;
    }

    s_valid = 1u;
    s_seq   = info[s_active].seq;
    s_cells = info[s_active].cells;
    s_unix  = info[s_active].calib_unix;
    s_t_x10 = info[s_active].t_calib_x10;
    s_dbg[2] = s_active;
}

uint8_t app_calib_valid(void) { return s_valid; }

void app_calib_info(app_calib_info_t *o)
{
    if (o == NULL) { return; }

    o->valid = s_valid;
    o->cells = s_cells;
    o->ocv_n = s_ocv_n;
    o->copy_used = (s_valid != 0u) ? s_active : 0xFFu;
    o->seq = s_seq;
    o->calib_unix = s_unix;
    o->t_calib_x10 = s_t_x10;
}

uint8_t app_calib_get_cell(uint8_t idx, uint32_t *k_ppm, int32_t *b_uv)
{
    if ((s_valid == 0u) || (idx >= (uint8_t)APP_CELL_COUNT)) { return 0u; }

    if (k_ppm != NULL) { *k_ppm = s_k[idx]; }
    if (b_uv  != NULL) { *b_uv  = s_b[idx]; }

    return 1u;
}

uint8_t app_calib_set_cell(uint8_t idx, uint32_t k_ppm, int32_t b_uv)
{
    if (idx >= (uint8_t)APP_CALIB_CELL_MAX) { return 1u; }

    /* ★ 合理性：k 落在 [0.9, 1.1]（ppm 口径）—— 超出这个范围的"标定"几乎一定是算错了。
     *   ⚠️ 这不是门限、是**输入合理性**；区间口径写在 §8 协议 (2) 的"1 ppm ≈ 3.65 µV"那一行旁。 */
    if ((k_ppm < 900000u) || (k_ppm > 1100000u)) { return 2u; }

    s_k[idx] = k_ppm;
    s_b[idx] = b_uv;

    return 0u;
}

uint8_t app_calib_set_ocv(uint8_t n, const uint16_t *soc_x10, const uint16_t *ocv_mv)
{
    uint8_t i;

    if ((n < 2u) || (n > (uint8_t)APP_CALIB_OCV_MAX) || (soc_x10 == NULL) || (ocv_mv == NULL)) { return 1u; }

    for (i = 0u; i < n; i++)
    {
        s_ocv_soc[i] = soc_x10[i];
        s_ocv_mv[i]  = ocv_mv[i];

        if ((i > 0u) && (soc_x10[i] <= soc_x10[i - 1u])) { return 2u; }      /* ★ 必须按 soc 升序 */
    }

    s_ocv_n = n;

    return 0u;
}

void app_calib_get_ocv(uint8_t *n, uint16_t *soc_x10, uint16_t *ocv_mv)
{
    uint8_t i;

    if (n != NULL) { *n = s_ocv_n; }

    if (soc_x10 != NULL) { for (i = 0u; i < (uint8_t)APP_CALIB_OCV_MAX; i++) { soc_x10[i] = s_ocv_soc[i]; } }
    if (ocv_mv  != NULL) { for (i = 0u; i < (uint8_t)APP_CALIB_OCV_MAX; i++) { ocv_mv[i]  = s_ocv_mv[i];  } }
}

void app_calib_clear_ocv(void)
{
    uint8_t i;

    for (i = 0u; i < (uint8_t)APP_CALIB_OCV_MAX; i++) { s_ocv_soc[i] = 0u; s_ocv_mv[i] = 0u; }

    s_ocv_n = 0u;
}

void app_calib_commit_request(void) { s_pending = 1u; }

void app_calib_pending(uint8_t *pending, uint32_t *seq)
{
    if (pending != NULL) { *pending = s_pending; }
    if (seq != NULL) { *seq = s_seq; }
}

void app_calib_diag(uint8_t *out, uint8_t n)
{
    uint8_t i;

    if (out == NULL) { return; }

    for (i = 0u; (i < n) && (i < 4u); i++) { out[i] = s_dbg[i]; }
}

/**
 * @brief       落盘：**整页读 → 只改目标副本 → 整页写**
 * @note        ★★ 为什么必须先读整页：`stmflash_write()` 是"整页擦 + 整页写回"，
 *              若只把目标副本放进缓冲、其余填 0xFF，**另一份副本会被抹掉**（两份都写在同一页里）。
 *              ⇒ 顺序钉死：`stmflash_read(整页)` → 覆盖目标副本 → `stmflash_write(整页)`。
 */
uint8_t app_calib_poll(uint8_t in_safe_off)
{
    uint8_t  target;
    uint32_t new_seq;
    uint16_t chk[CAL_REC_HW];

    if (s_pending == 0u) { s_last_rc = 0u; return 0u; }

    if (in_safe_off == 0u) { s_last_rc = 1u; return 1u; }        /* 排队等着，⛔ 不改任何状态 */

    target = (uint8_t)(s_active ^ 1u);
    new_seq = s_seq + 1u;

    /* 1) 整页读（保住另一份副本） */
    stmflash_read(APP_CALIB_ADDR, s_page, (uint16_t)(APP_FLASH_PAGE_SIZE / 2u));

    /* 2) 只覆盖目标副本 */
    {
        uint16_t rec[CAL_REC_HW];
        uint16_t i;

        calib_pack(rec, new_seq, (uint8_t)APP_CELL_COUNT, s_unix, s_t_x10);

        for (i = 0u; i < (uint16_t)CAL_REC_HW; i++)
        {
            s_page[calib_copy_off(target) + i] = rec[i];
        }
    }

    /* 3) 整页写 */
    stmflash_write(APP_CALIB_ADDR, s_page, (uint16_t)(APP_FLASH_PAGE_SIZE / 2u));

    /* 4) ★ 回读校验：⛔ 不信任"写成功" */
    stmflash_read(APP_CALIB_ADDR + (calib_copy_off(target) * 2u), chk, (uint16_t)CAL_REC_HW);

    if (calib_parse(chk, NULL) != CAL_ST_OK)
    {
        s_pending = 0u;
        s_last_rc = 2u;
        s_dbg[3] = 2u;
        return 2u;                              /* 目标副本没写成功 ⇒ 原活动副本**原封不动** */
    }

    s_active = target;
    s_seq = new_seq;
    s_valid = 1u;
    s_pending = 0u;
    s_last_rc = 0u;
    s_dbg[2] = target;
    s_dbg[3] = 0u;

    return 0u;
}

/* ===================== ★★ 离线注入自检（⛔ 不碰 Flash） ===================== */
uint16_t app_calib_selftest(void)
{
    uint16_t rec[CAL_REC_HW];
    uint8_t  rc;
    app_calib_info_t info;
    uint16_t soc[3];
    uint16_t mv[3];


    /* --- 用例 1：编→解 往返一致（写 4 串 k/b 与 3 个 OCV 点） --- */
    app_calib_init();
    s_k[0] = 1000000u; s_b[0] = 0;
    s_k[1] = 1000500u; s_b[1] = -1200;
    s_k[2] = 999000u;  s_b[2] = 800;
    s_k[3] = 1000000u; s_b[3] = 0;
    soc[0] = 100u; mv[0] = 3280u;
    soc[1] = 500u; mv[1] = 3300u;
    soc[2] = 900u; mv[2] = 3350u;
    if (app_calib_set_ocv(3u, soc, mv) != 0u) { s_selftest_rc = 1u; return 1u; }

    calib_pack(rec, 7u, (uint8_t)APP_CELL_COUNT, 0x11223344u, 250);

    memset(s_k, 0, sizeof(s_k));
    memset(s_b, 0, sizeof(s_b));
    s_ocv_n = 0u;

    memset(&info, 0, sizeof(info));
    rc = calib_parse(rec, &info);
    if (rc != CAL_ST_OK) { s_selftest_rc = 1u; return 1u; }
    if ((info.seq != 7u) || (info.calib_unix != 0x11223344u) || (info.t_calib_x10 != 250)) { s_selftest_rc = 1u; return 1u; }
    if ((s_k[1] != 1000500u) || (s_b[1] != -1200) || (s_k[2] != 999000u) || (s_b[2] != 800)) { s_selftest_rc = 1u; return 1u; }
    if ((s_ocv_n != 3u) || (s_ocv_mv[2] != 3350u)) { s_selftest_rc = 1u; return 1u; }

    /* --- 用例 2：★ CRC 覆盖**判据字段** ⇒ 改"序号"必须被发现 --- */
    calib_pack(rec, 7u, (uint8_t)APP_CELL_COUNT, 0u, 0);
    rec[CAL_SEQ_LO] = (uint16_t)(rec[CAL_SEQ_LO] ^ 0x0001u);      /* 翻一位 */
    if (calib_parse(rec, NULL) != CAL_ST_CORRUPT) { s_selftest_rc = 2u; return 2u; }

    /* --- 用例 3：★ 串数不符 ⇒ 整表拒用（"不适用"，⛔ 不是"坏"） --- */
    calib_pack(rec, 1u, (uint8_t)(APP_CELL_COUNT + 1u), 0u, 0);
    if (calib_parse(rec, NULL) != CAL_ST_BADCELLS) { s_selftest_rc = 3u; return 3u; }

    /* --- 用例 4：空白 ⇒ 不可用（★ 回退全局，⛔ 不拒绝启动） --- */
    {
        uint16_t i;
        for (i = 0u; i < (uint16_t)CAL_REC_HW; i++) { rec[i] = 0xFFFFu; }
    }
    if (calib_parse(rec, NULL) != CAL_ST_BLANK) { s_selftest_rc = 4u; return 4u; }

    /* --- 用例 5：版本是别的 ⇒ OLDVER（⛔ 不判成"坏"） --- */
    calib_pack(rec, 1u, (uint8_t)APP_CELL_COUNT, 0u, 0);
    rec[CAL_VER] = (uint16_t)(APP_CALIB_VER + 1u);
    rec[CAL_CRC] = calib_crc(rec);
    if (calib_parse(rec, NULL) != CAL_ST_OLDVER) { s_selftest_rc = 5u; return 5u; }

    /* --- 用例 6：OCV 升序校验（乱序必须被拒） --- */
    app_calib_init();
    soc[0] = 500u; mv[0] = 3300u;
    soc[1] = 100u; mv[1] = 3280u;
    soc[2] = 900u; mv[2] = 3350u;
    if (app_calib_set_ocv(3u, soc, mv) != 2u) { s_selftest_rc = 6u; return 6u; }

    s_selftest_rc = 0u;
    return 0u;
}

uint16_t app_calib_selftest_result(void) { return s_selftest_rc; }
