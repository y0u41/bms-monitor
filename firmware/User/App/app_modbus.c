/**
 ****************************************************************************************************
 * @file        app_modbus.c
 * @brief       Modbus RTU 从机实现（CRC16 / 五个功能码 / §6.1 的保持寄存器与线圈映射）
 * @note        文件头（实现/未实现清单、两条边界、C-100 的关系）见 `app_modbus.h`；
 *              物理层（USART2 + PD7 + DMA/IDLE/TC）见 `Drivers/BSP/RS485/`。
 *              本文件里 ⛔ 不得出现任何寄存器名、引脚名、DMA 通道号（S15 / G7-① 机械检查）。
 ****************************************************************************************************
 */

#include "./App/app_modbus.h"
#include "./App/app_config.h"
#include "./App/app_data.h"
#include "./App/app_protect.h"
#include "./App/app_log.h"
#include "./App/app_ui.h"
#include "./BSP/RS485/rs485.h"
#include "./BSP/RTC/rtc.h"
#include "./BSP/ACTUATOR/actuator.h"     /* ★ 只为一个用途：0x0020 低三位的位序来源（见下面的断言） */
#include <string.h>

/* ★★ 0x0020 低三位**不许有两套位序**（`app_log.h` 的 `EV_MOS_CLOSE` 注记过一次"写反过"的历史）⇒
 *    把"协议位"与"执行器状态位"的一致性做成**编译期断言**，而不是一条注释。
 *    ⛔ 将来谁改了 `actuator.h` 的 `ACT_ST_*`，这里当场编译不过。 */
APP_STATIC_ASSERT(MB_ACT_BIT_CHG == ACT_ST_CHG, mb_act_bit_chg_mismatch);
APP_STATIC_ASSERT(MB_ACT_BIT_DSG == ACT_ST_DSG, mb_act_bit_dsg_mismatch);
APP_STATIC_ASSERT(MB_ACT_BIT_PRE == ACT_ST_PRE, mb_act_bit_pre_mismatch);

/* CRC-16/MODBUS 的标准检查值（对 ASCII "123456789"）：`app_modbus_selftest()` 用它做已知答案判据 */
#define MB_CRC16_KAT            0x4B37u

/* Modbus 的协议上限（规约常数，⛔ 不是可调参数）：0x03/0x06 一次最多 125 个寄存器、0x10 最多 123 个、
 * 0x01/0x05 的线圈一次最多 2000 个。写在这里是因为它们**属于规约**而不属于本项目配置。 */
#define MB_MAX_READ_REGS        125u
#define MB_MAX_WRITE_REGS       123u
#define MB_MAX_COILS            2000u

/* ===================== 模块状态 ===================== */
static uint8_t  s_addr          = APP_MB_ADDR_DEFAULT;
static uint8_t  s_baud_code     = APP_MB_BAUD_CODE_DEFAULT;
static uint8_t  s_baud_pending  = 0xFFu;            /* 0xFF = 没有待切换的波特率 */

static uint8_t  s_frame[APP_MB_ADU_MAX];            /* 收到的请求（从物理层拷过来） */
static uint8_t  s_out[APP_MB_ADU_MAX];              /* 待发的响应 */

static bms_snapshot_t s_snap;
static uint8_t  s_snap_loaded;                      /* 帧内只读一次快照 */
static uint8_t  s_snap_rc;

/* ★ `0x0028~0x002D`（年/月/日/时/分/秒）**必须来自同一个瞬时**，所以帧内只换算一次并缓存。
 * ⚠️ 第一版写的是「每一格各调一次 `rtc_get_time()` 再读全局 `calendar`」⇒ 两个后果都不报错：
 *   ① 六格可能跨秒/跨分边界（上位机自己就能看出 `23:59:59` 与 `00:00:00` 混在同一帧里，
 *      而固件零现象）；② 一次请求里对外**写同一个全局 `calendar` 达 12 次**（校验一遍 + 填值一遍），
 *      而 `calendar` 的另一个写者是 `app_log_add()`（多任务）⇒ 等于给一个共享全局再加一个写者。
 *   ⇒ 改成：`app_log_now_unix()`（`app_log.h` 明确写的"**日志与 0x0202 协议共用**的唯一换算口"）
 *      ＋ `rtc_unix2time()` 换进**局部**结构 ⇒ ★ 本文件从此**不直接读写 `calendar`**，
 *      且这六格与 `0x0202` 的时间戳来自**同一次换算**。 */
static uint8_t  s_time_loaded;
static uint16_t s_time_hw[6];                       /* 年/月/日/时/分/秒 */

/* 日志读协议的缓存（§6.1 的 0x0200~0x0241）：**必须先写 0x0200 再读 0x0201/0x0202…**，
 * 所以这里必须把批次缓存下来 —— ⛔ 不能等读到 0x0202 时再去翻 Flash（两次读之间日志还在前进，
 * 那样返回的就是"拼接起来的历史"，而且没有任何报错能让上位机看出来）。 */
static uint16_t s_log_req;
static uint16_t s_log_n;
static uint16_t s_log_cache[APP_MB_LOG_BATCH * 4u];

/* 取证计数（⛔ 不参与任何判据；口径见 `app_modbus.h`） */
static uint32_t s_c_rx, s_c_answered, s_c_crc, s_c_mismatch, s_c_exc, s_c_txerr;

/* ===================== 小工具 ===================== */

static uint16_t mb_rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void mb_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

/**
 * @brief       CRC-16/MODBUS（多项式 0x8005 反射形式 = 0xA001，初值 0xFFFF，无最终异或）
 * @note        返回的是**寄存器口径的数值**；上线时的字节序是**低字节在前**（Modbus 规约），
 *              两者在 `mb_append_crc()` / 校验处各转换一次，⛔ 不要把这个函数改成"已经换过序"。
 */
static uint16_t mb_crc16(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;
    uint8_t  b;

    for (i = 0u; i < n; i++)
    {
        crc ^= (uint16_t)d[i];

        for (b = 0u; b < 8u; b++)
        {
            if ((crc & 0x0001u) != 0u)
            {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            }
            else
            {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

/** 在 buf[len] 处追加 CRC（**低字节在前**）并返回新长度 */
static uint16_t mb_append_crc(uint8_t *buf, uint16_t len)
{
    uint16_t crc = mb_crc16(buf, len);

    buf[len]      = (uint8_t)(crc & 0xFFu);
    buf[len + 1u] = (uint8_t)(crc >> 8);

    return (uint16_t)(len + 2u);
}

/** 组一条异常响应：[addr, fc|0x80, exc, crc] */
static uint16_t mb_exception(uint8_t addr, uint8_t fc, uint8_t exc, uint8_t *out)
{
    out[0] = addr;
    out[1] = (uint8_t)(fc | 0x80u);
    out[2] = exc;

    return mb_append_crc(out, 3u);
}

/** 帧内快照（只读一次；`app_data_read()` 的重试是有界的，不涉及硬件） */
static uint8_t mb_snap(void)
{
    if (s_snap_loaded == 0u)
    {
        s_snap_rc     = app_data_read(&s_snap);
        s_snap_loaded = 1u;
    }

    return s_snap_rc;
}

/** 从未更新过的时间戳返回一个"极大年龄"（饱和由调用方做） */
static uint32_t mb_age_ms(uint32_t tick, uint32_t now_ms)
{
    if (tick == 0u)
    {
        return 0xFFFFFFFFu;
    }

    return (uint32_t)(now_ms - tick);
}

/**
 * @brief       帧内一次性把 RTC 换算成"年/月/日/时/分/秒"六格
 * @note        ⚠️ `app_log_now_unix()` 在 RTC 未就绪时返回 **0**（`app_log.h` 的口径），
 *              此时六格全部写 **0** —— 年份 0 不是一个日期，上位机看到的就是"这一帧没有时间"；
 *              ⛔ **不得**改回 `HAL_GetTick()` 凑一个"看起来像"的数（那是把"我不知道"写成"我知道"）。
 */
static void mb_time_load(void)
{
    _calendar_obj t;
    uint32_t      ts;

    if (s_time_loaded != 0u)
    {
        return;
    }

    s_time_loaded = 1u;

    ts = app_log_now_unix();

    if (ts == 0u)
    {
        memset(s_time_hw, 0, sizeof(s_time_hw));
        return;
    }

    rtc_unix2time(ts, &t);

    s_time_hw[0] = t.year;
    s_time_hw[1] = (uint16_t)t.month;
    s_time_hw[2] = (uint16_t)t.date;
    s_time_hw[3] = (uint16_t)t.hour;
    s_time_hw[4] = (uint16_t)t.min;
    s_time_hw[5] = (uint16_t)t.sec;
}

/* ===================== §6.1 保持寄存器：读 ===================== */

/**
 * @brief       读一个保持寄存器
 * @param       a   寄存器地址（§6.1 的地址列）
 * @param       v   输出
 * @param       now_ms  当前毫秒（只有 0x0030 新鲜度用它）
 * @retval      1 = 该地址存在且本版已实现；0 = 不存在或未实现 ⇒ 调用方回异常码 0x02
 * @note        ★ 无效值口径（§4.4）：**能表达"不可知"的量一律回无效码，⛔ 不回 0**。
 *              0 在多数寄存器里都是一个**合法读数**（0mV / 0A / 0.0℃），
 *              而"0.0℃"与"我不知道温度"对保护判据是两个完全相反的前提。
 *              ⚠️ **例外（刻意）**：`0x0012` 电流与 `0x0010/0x0011` 组压**没有**无效码，
 *              它们的"不可信"由 `0x002F` 的 link 位表达（§4.4 的 `link_health` 就是为这件事存在的，
 *              而电流是 int16_t、`0xFFFF` 会被解读成 −1（−10mA），加个"无效码"反而制造歧义）。
 */
static uint8_t mb_reg_read(uint16_t a, uint16_t *v, uint32_t now_ms)
{
    const bms_raw_t     *r  = &s_snap.raw;
    const bms_derived_t *d  = &s_snap.der;
    const bms_alarm_t   *al = &s_snap.alarm;
    uint8_t  idx;
    uint8_t  bits = 0u;
    uint8_t  diag_rc = 0u;

    /* ---- 0x0000~0x000F：单体电压 1~16（mV；无效串 = 0xFFFF） ---- */
    if (a < 0x0010u)
    {
        *v = (((r->cell_valid & (uint16_t)(1u << a)) != 0u) ? r->cell_mv[a] : (uint16_t)APP_MV_INVALID);
        return 1u;
    }

    switch (a)
    {
        /* ---- 0x0010~0x0012：组压（AFE 主通道 / INA226 冗余）/ 电流 -*- */
        case 0x0010u: *v = r->pack_10mv;                              return 1u;
        case 0x0011u: *v = r->pack_10mv_aux;                          return 1u;
        case 0x0012u: *v = (uint16_t)r->curr_10ma;                    return 1u;

        /* ---- 0x0013~0x0014：温度 1~2（表面 / 环境，0.1℃） ---- */
        case 0x0013u:
        case 0x0014u:
        {
            idx = (uint8_t)(a - 0x0013u);
            *v = (((r->temp_valid & (uint16_t)(1u << idx)) != 0u)
                  ? (uint16_t)r->temp_x10[idx] : (uint16_t)APP_MV_INVALID);
            return 1u;
        }

        /* ---- 0x0015~0x0016：SOC / SOH（0.1%） ---- */
        case 0x0015u: *v = d->soc_x10;                                return 1u;
        case 0x0016u: *v = d->soh_x10;                                return 1u;

        /* ---- 0x0017~0x0019：最高 / 最低 / 压差（mV） ---- */
        case 0x0017u: *v = ((r->cell_valid != 0u) ? d->cell_max_mv : (uint16_t)APP_MV_INVALID); return 1u;
        case 0x0018u: *v = ((r->cell_valid != 0u) ? d->cell_min_mv : (uint16_t)APP_MV_INVALID); return 1u;
        case 0x0019u: *v = ((r->cell_valid != 0u) ? d->delta_mv    : (uint16_t)APP_MV_INVALID); return 1u;

        /* ---- 0x001A~0x001C：最高 / 最低 / 温差（0.1℃）
         * ★ 这一格正是 `Tech_Design.md` §4.4 注⑨ 登记的那笔债（"一个有效测点都没有时
         *   三个派生量延续初值 0 ⇒ 一上 Modbus 就是把 0.0℃ 报给上位机"）⇒ 本版按
         *   **与单体同一个纪律**补上无效码出口（`temp_valid == 0` ⇒ 0xFFFF）。 */
        case 0x001Au: *v = ((r->temp_valid != 0u) ? (uint16_t)d->temp_max_x10   : (uint16_t)APP_MV_INVALID); return 1u;
        case 0x001Bu: *v = ((r->temp_valid != 0u) ? (uint16_t)d->temp_min_x10   : (uint16_t)APP_MV_INVALID); return 1u;
        case 0x001Cu: *v = ((r->temp_valid != 0u) ? (uint16_t)d->temp_delta_x10 : (uint16_t)APP_MV_INVALID); return 1u;
        case 0x001Du: *v = (uint16_t)d->temp_rise_x10_per_min;         return 1u;

        /* ---- 0x001E / 0x001F：告警等级 / 故障位图（§6.2） ---- */
        case 0x001Eu: *v = (uint16_t)al->level;                        return 1u;
        case 0x001Fu: *v = al->fault_bits;                             return 1u;

        /* ---- 0x0020：执行器状态（位定义见 `app_modbus.h`） ---- */
        case 0x0020u:
            if (app_protect_mos_diag(&bits, &diag_rc) == 0u)
            {
                *v = MB_ACT_BIT_UNKN;           /* ★ 不可知就是不可知，⛔ 不渲染成"全断开" */
                return 1u;
            }
            *v = (uint16_t)bits;
            if (app_protect_state() == BMS_STATE_FAULT_LATCHED)
            {
                *v |= MB_ACT_BIT_LATCH;
            }
            if (al->hw_layer_state == BMS_HW_ACTED)
            {
                *v |= MB_ACT_BIT_HW_ACTED;
            }
            return 1u;

        /* ---- 0x0021：均衡状态位图。★ 恒 0 且**这不是"未实现"** —— 本版没有均衡功能（P2），
         *      "没有任何一串在均衡"是一个为真的读数（`app_modbus.h` 文件头 ⚠️ b)）。 ---- */
        case 0x0021u: *v = 0x0000u;                                    return 1u;

        /* ---- 0x0022~0x0027：累计充电/放电 mAh、循环次数（u32，高字在前） ---- */
        case 0x0022u: *v = (uint16_t)(d->chg_mah >> 16);               return 1u;
        case 0x0023u: *v = (uint16_t)(d->chg_mah & 0xFFFFu);           return 1u;
        case 0x0024u: *v = (uint16_t)(d->dis_mah >> 16);               return 1u;
        case 0x0025u: *v = (uint16_t)(d->dis_mah & 0xFFFFu);           return 1u;
        case 0x0026u: *v = (uint16_t)(d->cycles >> 16);                return 1u;
        case 0x0027u: *v = (uint16_t)(d->cycles & 0xFFFFu);            return 1u;

        /* ---- 0x0028~0x002D：RTC 年/月/日/时/分/秒 ----
         * ★ 六格来自**同一次换算**（`mb_time_load()` 帧内只跑一次），理由与第一版的错见模块状态区。 */
        case 0x0028u: mb_time_load(); *v = s_time_hw[0];               return 1u;
        case 0x0029u: mb_time_load(); *v = s_time_hw[1];               return 1u;
        case 0x002Au: mb_time_load(); *v = s_time_hw[2];               return 1u;
        case 0x002Bu: mb_time_load(); *v = s_time_hw[3];               return 1u;
        case 0x002Cu: mb_time_load(); *v = s_time_hw[4];               return 1u;
        case 0x002Du: mb_time_load(); *v = s_time_hw[5];               return 1u;

        /* ---- 0x002E：串数（★ 上位机据此自适应，值 = 编译期 `APP_CELL_COUNT`） ---- */
        case 0x002Eu: *v = (uint16_t)APP_CELL_COUNT;                   return 1u;

        /* ---- 0x002F：采样链路健康位图（`BMS_LINK_xxx`） ---- */
        case 0x002Fu: *v = (uint16_t)al->link_health;                  return 1u;

        /* ---- 0x0030：数据新鲜度 = **最旧那一路**距今秒数（★ 本轮补的定义，已同步 §6.1） ----
         * ★ 为什么要"最旧那一路"而不是 `tick_ms`：`tick_ms` 是 raw 帧整体心跳，
         *   而"电流通道死了、温度还活着"正是 §4.4 要让上位机看见的那类故障。
         *   ⛔ 若只报 `tick_ms`，一个已经僵住的电流通道会一直显示"新鲜"。
         * ⚠️ 参与的通道 = 本工程全部**有属主**的通道时间戳（`tick_ms` / `tick_ms_cell` /
         *   `tick_ms_curr` + 逐测点 `tick_ms_temp[0..ACTIVE-1]`）；任一格从未更新 ⇒ 0xFFFF。 ---- */
        case 0x0030u:
        {
            uint32_t worst = 0u;
            uint32_t a2;

            a2 = mb_age_ms(r->tick_ms, now_ms);      if (a2 > worst) { worst = a2; }
            a2 = mb_age_ms(r->tick_ms_cell, now_ms); if (a2 > worst) { worst = a2; }
            a2 = mb_age_ms(r->tick_ms_curr, now_ms); if (a2 > worst) { worst = a2; }

            for (idx = 0u; idx < (uint8_t)APP_BMS_TEMP_ACTIVE_COUNT; idx++)
            {
                a2 = mb_age_ms(r->tick_ms_temp[idx], now_ms);

                if (a2 > worst)
                {
                    worst = a2;
                }
            }

            *v = (worst > ((uint32_t)0xFFFEu * 1000u)) ? 0xFFFFu : (uint16_t)(worst / 1000u);
            return 1u;
        }

        /* ---- 0x0031 / 0x0032：降额请求状态 / 开关两端压差 ---- */
        case 0x0031u: *v = (uint16_t)al->derate_state;                 return 1u;
        case 0x0032u: *v = ((r->v_sw_valid != 0u) ? r->v_sw_10mv : (uint16_t)APP_MV_INVALID); return 1u;

        /* ---- 0x0200~0x0241：日志读协议（写 0x0200 之后才有内容） ---- */
        case 0x0200u: *v = s_log_req;                                  return 1u;
        case 0x0201u: *v = s_log_n;                                    return 1u;

        case 0x0300u: *v = MB_FW_VER_PACKED;                           return 1u;
        /* ⚠️ 0x0301 = 参数区版本 + CRC 状态：**参数区不存在** ⇒ 回 0xFFFF（无效），
         *     ⛔ 不回 0（回 0 等于说"参数区版本 0 且 CRC 通过"，那是一句假话）。 */
        case 0x0301u: *v = 0xFFFFu;                                    return 1u;
        case 0x0302u: *v = (uint16_t)s_addr;                           return 1u;
        case 0x0303u: *v = (uint16_t)s_baud_code;                      return 1u;
        case 0x0304u: *v = 0x0000u;                                    return 1u;   /* 无待提交命令 */
        case 0x0305u: *v = (uint16_t)((APP_LOG_FMT_VER << 8) | APP_LOG_SLOT_SIZE); return 1u;

        default:
            break;
    }

    /* 0x0202~0x0241：日志批次内容（越界的那部分地址不存在） */
    if ((a >= 0x0202u) && (a <= (uint16_t)(0x0201u + (APP_MB_LOG_BATCH * 4u))))
    {
        *v = s_log_cache[a - 0x0202u];
        return 1u;
    }

    return 0u;      /* 不存在或本版未实现 ⇒ 异常码 0x02（⛔ 绝不回 0） */
}

/* ===================== §6.1 保持寄存器：写 ===================== */

/**
 * @brief       写一个保持寄存器
 * @retval      0 = 成功；非 0 = 要回的异常码
 * @note        ★ 可写面只有三格（§6.1 原表就只标了三格 R/W）：
 *              `0x0200`（日志读请求）、`0x0302`（从机地址）、`0x0303`（波特率编码）。
 *              其余一律异常码 0x02 —— ⛔ 尤其**不得静默忽略**：一个"接受但没生效"的写
 *              在 Modbus 读回上与"生效了"完全同形（本模块文件头的边界 a 就是同一类）。
 */
static uint8_t mb_reg_write(uint16_t a, uint16_t v)
{
    switch (a)
    {
        case 0x0200u:
            /* 请求序号（0 = 最新一条）。★ 真正取数在下一次读 0x0201/0x0202… 之前完成，
             * 取不到就返回 0 条（⛔ 不回异常：越界序号本来就是"没有更多了"的正常语义）。 */
            s_log_n = 0u;
            {
                app_log_rec_t rec;
                uint16_t      i;

                s_log_req = v;

                for (i = 0u; i < APP_MB_LOG_BATCH; i++)
                {
                    if (app_log_get((uint16_t)(s_log_req + i), &rec) == 0u)
                    {
                        break;
                    }

                    s_log_cache[(i * 4u) + 0u] = (uint16_t)(rec.ts_unix >> 16);
                    s_log_cache[(i * 4u) + 1u] = (uint16_t)(rec.ts_unix & 0xFFFFu);
                    s_log_cache[(i * 4u) + 2u] = rec.event;
                    s_log_cache[(i * 4u) + 3u] = rec.code;
                    s_log_n++;
                }
            }
            return 0u;

        case 0x0302u:
            if ((v < APP_MB_ADDR_MIN) || (v > APP_MB_ADDR_MAX))
            {
                return MB_EXC_ILLEGAL_VALUE;        /* 0 是广播地址，⛔ 不能配成从机地址 */
            }
            s_addr = (uint8_t)v;
            /* ⚠️ 只改 RAM：掉电回到 `app_config.h` 的默认值（文件头边界 a，登记为债务） */
            return 0u;

        case 0x0303u:
            if (v > APP_MB_BAUD_CODE_MAX)
            {
                return MB_EXC_ILLEGAL_VALUE;
            }
            /* ★ **不当场切**：本次请求的响应还要用旧波特率发出去。
             *   切换动作排在"回帧发完"之后（`app_modbus_step()` 尾部）——
             *   ⛔ 当场切的后果 = 响应前半段用新波特率、后半段用旧 ⇒ 主机必然 CRC 错，
             *   而现象是"改波特率之后从机就失联了"，看起来像参数没生效。 */
            s_baud_pending = (uint8_t)v;
            return 0u;

        default:
            return MB_EXC_ILLEGAL_ADDR;
    }
}

/* ===================== §6.1 线圈 ===================== */

/**
 * @brief       读一个线圈
 * @retval      1 = 该线圈存在且本版已实现；0 = 未实现（⇒ 异常码 0x02）
 * @note        本版实现的线圈只有 `0x0000/0x0001`（允许放电/允许充电）。
 */
static uint8_t mb_coil_read(uint16_t a, uint16_t *v)
{
    uint8_t chg = 0u;
    uint8_t dsg = 0u;

    if (a > 0x0001u)
    {
        return 0u;      /* 0x0002 一键上报（WiFi = 加分项）/ 0x0004 解除锁定（需鉴权 = P2）未实现 */
    }

    app_protect_get_intent(&chg, &dsg);
    *v = ((a == 0x0000u) ? ((dsg != 0u) ? 1u : 0u) : ((chg != 0u) ? 1u : 0u));

    return 1u;
}

/**
 * @brief       写一个线圈
 * @retval      0 = 成功；非 0 = 要回的异常码
 * @note        ★★ **线圈只写"意图"**（§6.1 原文 + S9）：`0x0000/0x0001` 落到
 *              `app_protect_set_intent()`，真正的开关动作全部发生在 `app_protect_step()` 里。
 *              ⛔ 本文件里出现任何"直接闭合"都是 S9 违规，而 G7-⑦ 会当场判红。
 */
static uint8_t mb_coil_write(uint16_t a, uint16_t v)
{
    uint8_t chg = 0u;
    uint8_t dsg = 0u;
    uint8_t on;

    if ((v != 0xFF00u) && (v != 0x0000u))
    {
        return MB_EXC_ILLEGAL_VALUE;        /* 规约：写线圈只认 0xFF00 / 0x0000 */
    }

    on = ((v == 0xFF00u) ? 1u : 0u);

    switch (a)
    {
        case 0x0000u:                       /* 允许放电 */
            app_protect_get_intent(&chg, &dsg);
            app_protect_set_intent(chg, on);
            return 0u;

        case 0x0001u:                       /* 允许充电 */
            app_protect_get_intent(&chg, &dsg);
            app_protect_set_intent(on, dsg);
            return 0u;

        case 0x0003u:                       /* 清除本地告警 = 等价短按 KEY0（⛔ 不解除三级锁定） */
            if (on != 0u)
            {
                /* ★ 走界面/告警侧的**唯一**出口（`app_ui_set_ack_hook()` 的注册点），
                 *   ⛔ 不在这里直接抓一个静音函数 —— 那会变成第二条通路，
                 *   两条通路今天恰好等价、明天一定不等价（C-97 F1 同族）。 */
                (void)app_ui_request_ack();
            }
            return 0u;

        default:
            return MB_EXC_ILLEGAL_ADDR;     /* 0x0002 / 0x0004 未实现（见 `mb_coil_read`） */
    }
}

/* ===================== 功能码 ===================== */

static uint16_t mb_fc_read_holding(const uint8_t *req, uint8_t *out, uint8_t *exc, uint32_t now_ms)
{
    uint16_t start = mb_rd16(&req[2]);
    uint16_t qty   = mb_rd16(&req[4]);
    uint16_t i;
    uint16_t v = 0u;

    if ((qty == 0u) || (qty > MB_MAX_READ_REGS))
    {
        *exc = MB_EXC_ILLEGAL_VALUE;
        return 0u;
    }

    /* ★ 先整体校验再写响应：Modbus 规定"任一地址非法 ⇒ 整个请求回异常码，**不返回部分数据**"。
     *   ⛔ 不许边查边填（那会返回一个缺了后半截的响应，而长度字段还写着满长度）。
     * ⚠️ **代价如实写下来**：为了满足上面这条，每个寄存器被算**两遍**（校验一遍 + 填值一遍）。
     *   两遍结果**必然相同** —— 快照（`s_snap`）与 RTC 六格（`mb_time_load()`）都是**帧内缓存**，
     *   而 `now_ms` 是入参常量 ⇒ 不存在"校验时合法、填值时变了"的窗口。
     *   ⛔ 不许为此改成"边查边填 + 出错时截断"：那会把"整帧异常码"退化成"半帧数据"，
     *   而上位机没有任何办法区分它和一个真的短响应。 */
    for (i = 0u; i < qty; i++)
    {
        if (mb_reg_read((uint16_t)(start + i), &v, now_ms) == 0u)
        {
            *exc = MB_EXC_ILLEGAL_ADDR;
            return 0u;
        }
    }

    out[0] = req[0];
    out[1] = MB_FC_READ_HOLDING;
    out[2] = (uint8_t)(qty * 2u);

    for (i = 0u; i < qty; i++)
    {
        (void)mb_reg_read((uint16_t)(start + i), &v, now_ms);
        mb_wr16(&out[3u + (i * 2u)], v);
    }

    return mb_append_crc(out, (uint16_t)(3u + (qty * 2u)));
}

static uint16_t mb_fc_read_coils(const uint8_t *req, uint8_t *out, uint8_t *exc)
{
    uint16_t start = mb_rd16(&req[2]);
    uint16_t qty   = mb_rd16(&req[4]);
    uint16_t i;
    uint16_t v = 0u;
    uint8_t  nbytes;

    if ((qty == 0u) || (qty > MB_MAX_COILS))
    {
        *exc = MB_EXC_ILLEGAL_VALUE;
        return 0u;
    }

    for (i = 0u; i < qty; i++)
    {
        if (mb_coil_read((uint16_t)(start + i), &v) == 0u)
        {
            *exc = MB_EXC_ILLEGAL_ADDR;
            return 0u;
        }
    }

    nbytes = (uint8_t)((qty + 7u) / 8u);
    out[0] = req[0];
    out[1] = MB_FC_READ_COILS;
    out[2] = nbytes;
    memset(&out[3], 0, (size_t)nbytes);

    for (i = 0u; i < qty; i++)
    {
        (void)mb_coil_read((uint16_t)(start + i), &v);

        if (v != 0u)
        {
            out[3u + (i / 8u)] |= (uint8_t)(1u << (i % 8u));
        }
    }

    return mb_append_crc(out, (uint16_t)(3u + nbytes));
}

static uint16_t mb_fc_write_one(const uint8_t *req, uint8_t *out, uint8_t *exc)
{
    uint16_t a = mb_rd16(&req[2]);
    uint16_t v = mb_rd16(&req[4]);
    uint8_t  e = mb_reg_write(a, v);

    if (e != 0u)
    {
        *exc = e;
        return 0u;
    }

    /* 规约：正常的 0x06 响应 = **原样回显请求的前 6 个字节** + CRC */
    memcpy(out, req, 6u);

    return mb_append_crc(out, 6u);
}

static uint16_t mb_fc_write_coil(const uint8_t *req, uint8_t *out, uint8_t *exc)
{
    uint16_t a = mb_rd16(&req[2]);
    uint16_t v = mb_rd16(&req[4]);
    uint8_t  e = mb_coil_write(a, v);

    if (e != 0u)
    {
        *exc = e;
        return 0u;
    }

    memcpy(out, req, 6u);

    return mb_append_crc(out, 6u);
}

static uint16_t mb_fc_write_multi(const uint8_t *req, uint8_t *out, uint8_t *exc)
{
    uint16_t start = mb_rd16(&req[2]);
    uint16_t qty   = mb_rd16(&req[4]);
    uint8_t  bc    = req[6];
    uint16_t i;
    uint8_t  e;

    if ((qty == 0u) || (qty > MB_MAX_WRITE_REGS) || ((uint16_t)bc != (qty * 2u)))
    {
        *exc = MB_EXC_ILLEGAL_VALUE;
        return 0u;
    }

    /* ★ 逐个写，**遇错即停并回异常码**（规约允许"部分已执行"，但本版三格可写面都是
     *   幂等赋值 ⇒ 不存在"写了一半无法回退"的状态；这一点必须写在这里，否则下一个人
     *   会以为本版实现了事务语义）。 */
    for (i = 0u; i < qty; i++)
    {
        e = mb_reg_write((uint16_t)(start + i), mb_rd16(&req[7u + (i * 2u)]));

        if (e != 0u)
        {
            *exc = e;
            return 0u;
        }
    }

    out[0] = req[0];
    out[1] = MB_FC_WRITE_MULTI;
    mb_wr16(&out[2], start);
    mb_wr16(&out[4], qty);

    return mb_append_crc(out, 6u);
}

/* ===================== 帧分发 ===================== */

/**
 * @brief       处理一帧请求，产出响应
 * @retval      响应字节数；**0 = 保持沉默**（CRC 错 / 地址不是本机 / 广播 / 短到不成帧）
 * @note        ★ "沉默"是本模块最重要的一种输出：Modbus 规定从机对**校验不过**的帧
 *              必须不作答（否则总线上会出现两个响应）。⛔ 不得为了"好调试"改成回异常码——
 *              那会让一个噪声帧把主机的重试逻辑搅乱。
 */
static uint16_t mb_handle(const uint8_t *req, uint16_t reqlen, uint8_t *out, uint32_t now_ms)
{
    uint8_t  addr;
    uint8_t  fc;
    uint8_t  exc = 0u;
    uint16_t crc;
    uint16_t n = 0u;

    if ((reqlen < 4u) || (reqlen > APP_MB_ADU_MAX))
    {
        return 0u;
    }

    crc = (uint16_t)(((uint16_t)req[reqlen - 1u] << 8) | (uint16_t)req[reqlen - 2u]);

    if (mb_crc16(req, (uint16_t)(reqlen - 2u)) != crc)
    {
        s_c_crc++;
        return 0u;                      /* ⛔ 校验不过一律沉默 */
    }

    addr = req[0];

    if ((addr != s_addr) && (addr != MB_ADDR_BROADCAST))
    {
        s_c_mismatch++;
        return 0u;                      /* 不是本机也不是广播 ⇒ 沉默（别人的帧） */
    }

    fc = req[1];
    s_c_rx++;

    /* ★ 快照只在本帧内读一次：一份请求里的多个寄存器必须来自**同一帧快照**，
     *   否则同一响应里会出现"一半新一半旧"的组合（`app_data.h` 的撕裂帧口径）。
     *   ★ RTC 六格同理（`mb_time_load()`），两个"帧内只算一次"的闸门一起复位。 */
    s_snap_loaded = 0u;
    s_time_loaded = 0u;

    switch (fc)
    {
        case MB_FC_READ_HOLDING:
            if (reqlen != 8u) { exc = MB_EXC_ILLEGAL_VALUE; break; }
            if (mb_snap() == 2u) { exc = MB_EXC_DEVICE_FAILURE; break; }
            n = mb_fc_read_holding(req, out, &exc, now_ms);
            break;

        case MB_FC_READ_COILS:
            if (reqlen != 8u) { exc = MB_EXC_ILLEGAL_VALUE; break; }
            n = mb_fc_read_coils(req, out, &exc);
            break;

        case MB_FC_WRITE_ONE:
            if (reqlen != 8u) { exc = MB_EXC_ILLEGAL_VALUE; break; }
            n = mb_fc_write_one(req, out, &exc);
            break;

        case MB_FC_WRITE_COIL:
            if (reqlen != 8u) { exc = MB_EXC_ILLEGAL_VALUE; break; }
            n = mb_fc_write_coil(req, out, &exc);
            break;

        case MB_FC_WRITE_MULTI:
            if ((reqlen < 11u) || ((uint16_t)req[6] != (uint16_t)(reqlen - 9u)))
            {
                exc = MB_EXC_ILLEGAL_VALUE;
                break;
            }
            n = mb_fc_write_multi(req, out, &exc);
            break;

        default:
            exc = MB_EXC_ILLEGAL_FUNC;
            break;
    }

    if (exc != 0u)
    {
        s_c_exc++;

        /* ⛔ 广播（地址 0）**不回答**，异常也一样（规约：广播无响应） */
        if (addr == MB_ADDR_BROADCAST)
        {
            return 0u;
        }

        return mb_exception(addr, fc, exc, out);
    }

    if (addr == MB_ADDR_BROADCAST)
    {
        return 0u;                      /* 广播：执行了、但不回答 */
    }

    if (n != 0u)
    {
        s_c_answered++;
    }

    return n;
}

/* ===================== 对外接口 ===================== */

uint8_t app_modbus_init(uint32_t baud)
{
    s_addr      = APP_MB_ADDR_DEFAULT;
    s_baud_code = APP_MB_BAUD_CODE_DEFAULT;
    s_log_req   = 0u;
    s_log_n     = 0u;
    s_c_rx = s_c_answered = s_c_crc = s_c_mismatch = s_c_exc = s_c_txerr = 0u;

    return rs485_init(baud);
}

void app_modbus_step(uint32_t now_ms)
{
    uint16_t n;
    uint16_t rn;

    /* ---- 1) 待切换的波特率：**必须排在取帧之前、回帧之后的那一拍**生效 ----
     * 为什么排在最前：`s_baud_pending` 是上一拍"回帧已经启动"之后设的（见尾部），
     * 而本拍开始时上一帧的 DMA 早已发完（10ms 一拍、最长帧 133ms ⇒ 见下面的忙判据）。 */
    if ((s_baud_pending != 0xFFu) && (rs485_tx_busy() == 0u))
    {
        uint32_t b = APP_MB_BAUD_19200;

        if (s_baud_pending == APP_MB_BAUD_CODE_9600)  { b = APP_MB_BAUD_9600;  }
        if (s_baud_pending == APP_MB_BAUD_CODE_38400) { b = APP_MB_BAUD_38400; }

        if (rs485_set_baud(b) == RS485_OK)
        {
            s_baud_code    = s_baud_pending;
            s_baud_pending = 0xFFu;
        }
    }

    /* ---- 2) 上一次的响应还没发完 ⇒ 本拍什么都不做 ----
     * ⚠️ 这是"必须等"而不是"可以等"：同一时刻只允许一个方向占用总线（半双工）。
     *    ⛔ 不得在这里开新帧的解析 —— 那会在 DE 还是高的时候去读 RX（读到的是自己）。 */
    if (rs485_tx_busy() != 0u)
    {
        return;
    }

    /* ---- 3) 取一帧 ---- */
    n = rs485_rx_take(s_frame, (uint16_t)sizeof(s_frame));

    if (n == 0u)
    {
        return;
    }

    /* ---- 4) 处理并回帧 ---- */
    rn = mb_handle(s_frame, n, s_out, now_ms);

    if (rn == 0u)
    {
        return;
    }

    if (rs485_tx_start(s_out, rn) != RS485_OK)
    {
        s_c_txerr++;                    /* ⛔ 不回帧就是"从机没应答"，必须计数（否则现场无法归因） */
    }
}

uint8_t app_modbus_selftest(void)
{
    /* CRC-16/MODBUS 的标准检查值（已知答案判据，⛔ 不需要任何外部设备） */
    static const uint8_t kat[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };

    uint8_t  req[8];
    uint16_t n;
    uint16_t crc;
    uint32_t keep[6];

    if (mb_crc16(kat, 9u) != MB_CRC16_KAT)
    {
        return 1u;
    }

    /* ★ 第二项：合成一条"读 0x0000 起 2 个保持寄存器"的请求，**走同一条分发与响应路径**。
     *   ⛔ 只校对**结构**（地址 / 功能码 / 字节数字段 / CRC），不校对内容 ——
     *   内容来自快照，那是另一条判据（`app_data` 的契约级证据）。
     *   ⚠️ 本项会走一次真实的计数器路径 ⇒ 先把六个取证计数器存起来、测完还原，
     *      否则"基线"从一开始就是脏的（`Build.md` §8.27 的基线纪律）。 */
    keep[0] = s_c_rx;  keep[1] = s_c_answered; keep[2] = s_c_crc;
    keep[3] = s_c_mismatch; keep[4] = s_c_exc;  keep[5] = s_c_txerr;

    req[0] = s_addr;
    req[1] = MB_FC_READ_HOLDING;
    mb_wr16(&req[2], 0x0000u);
    mb_wr16(&req[4], 2u);
    (void)mb_append_crc(req, 6u);

    n = mb_handle(req, 8u, s_out, 0u);

    s_c_rx = keep[0];  s_c_answered = keep[1]; s_c_crc = keep[2];
    s_c_mismatch = keep[3]; s_c_exc = keep[4];  s_c_txerr = keep[5];

    /* 期望响应 = 地址(1) + 功能码(1) + 字节数(1) + 2×2 字节数据 + CRC(2) = 9 */
    if (n != 9u)
    {
        return 2u;
    }

    if ((s_out[0] != s_addr) || (s_out[1] != MB_FC_READ_HOLDING) || (s_out[2] != 4u))
    {
        return 2u;
    }

    crc = (uint16_t)(((uint16_t)s_out[n - 1u] << 8) | (uint16_t)s_out[n - 2u]);

    if (mb_crc16(s_out, (uint16_t)(n - 2u)) != crc)
    {
        return 2u;
    }

    return 0u;
}

/* ===================== 取证出口（⛔ 不参与任何判据） ===================== */
uint32_t app_modbus_rx_frames(void)     { return s_c_rx; }
uint32_t app_modbus_answered(void)      { return s_c_answered; }
uint32_t app_modbus_crc_errors(void)    { return s_c_crc; }
uint32_t app_modbus_addr_mismatch(void) { return s_c_mismatch; }
uint32_t app_modbus_exceptions(void)    { return s_c_exc; }
uint32_t app_modbus_tx_errors(void)     { return s_c_txerr; }
uint8_t  app_modbus_addr(void)          { return s_addr; }
uint8_t  app_modbus_baud_code(void)     { return s_baud_code; }
