/**
 ****************************************************************************************************
 * @file        app_ui.c
 * @brief       BMS 本地界面：总况 / 单体电压 / 事件 三页 + 按键（短按清告警、长按解除锁定、翻页）
 * @note        Build.md D2 的交付物。验收口径：**3 页可切换、按键有效、数据先填假值**。
 *              假值来自 app_data_fake_step()，界面这一侧的代码从 D3 起**一行都不用改** ——
 *              它读的一直是 `app_data_read()` 的快照，换的是快照的生产者。
 *
 * @note        ★ 三条纪律在本文件里的落点（对应 AGENTS.md）：
 *              - S9  开关动作只允许出现在 app_protect.c：所以"长按 5s 解除锁定"在这里
 *                    只是**发出请求**（unlock hook），界面不写任何执行器、也不改锁定标志。
 *                    hook 未注册时（D5 之前的现状）只在串口打一行"已发出请求"，
 *                    ⛔ 不允许"反正没人处理，界面顺手把标志清了" —— 那等于把 S9 绕过去。
 *              - S11 解除锁定的时长判据用 `APP_CTR_LATCH_HOLD_MS`（app_config.h §12.10），
 *                    ⛔ 不写字面量 5000（S14）。
 *              - S15/G6  这里没有任何 AFE 寄存器名、也没有执行器引脚宏；遍历电芯一律
 *                    `APP_CELL_COUNT`，无效串显示 "----"（★ 不是显示 0，0 是"电芯被短接"的合法读数）。
 *
 * @note        ★ 界面**不做任何保护判断**（§4.2）。页面上看到的等级、故障位、锁定状态
 *              全部是从快照里读来的结论，界面只是复述。上色也只按 `level` 与
 *              `link_health`，⛔ 不在这里比较门限。
 *
 * @note        刷新策略：整屏只在"换页 / 底色变化 / 字号变化"时清屏重画，
 *              其余时间逐行比对文本、只重写变了的那一行 —— 无闪烁，且把 FSMC 总线占用
 *              压到最低（本任务优先级最低，不得拖慢采集与保护，FR-HMI-04）。
 ****************************************************************************************************
 */

#include "./App/app_ui.h"
#include "./App/app_main.h"      /* ★ 启动自检汇总出口 app_selfcheck_rc() */
#include "./App/app_acq.h"      /* ★ Self 行的实时通道状态（*_stat） */
#include "./App/app_data.h"
#include "./App/app_config.h"
#include "./App/app_env_alarm.h"
#include "./App/app_tasks.h"      /* ⏳ 调试行专用（D9 串口命令入口建好后撤） */
#if APP_ENABLE_LOG
#include "./App/app_log.h"
#endif
#include "./BSP/LCD/lcd.h"
#include "./BSP/KEY/key.h"
#include <stdio.h>
#include <string.h>

/* ==================== 版式 ==================== */
#define UI_X                6           /* 左边距 */
#define UI_LINE_MAX         60          /* 单行缓存字符数上限 */
#define UI_ROW_BUF          12          /* 缓存多少行（含标题与页脚） */
#define UI_BODY_ROWS        8           /* 版式按"最多 8 行正文"挑字号 */
#define UI_GAP              4           /* 行间距 */

/* 按键消抖：连续 N 次采到同一电平才认，配下面的 10ms 采样下限 ⇒ 20ms。
 * ⚠️ 采样下限是必需的，不是可选项：本函数在联网等待期间会被 app_poll() 以远快于
 *    20ms 的频率调进来，若"每次都算一次采样"，N 次可能落在几毫秒之内 ——
 *    抖动窗还没过就判定"电平稳定"，消抖等于没做。
 *  D5 迁到 TaskUi 时这条与 app_ui.h 头注第 2 条一起处理（200ms 抓不到短按）。 */
#define UI_KEY_SCAN_MS      10
#define UI_KEY_DEBOUNCE_N   2

/* ==================== 状态 ==================== */
static uint8_t        s_page        = UI_PAGE_OVERVIEW;
static uint8_t        s_font        = 16;     /* 正文字号 */
static uint8_t        s_title_font  = 24;     /* 标题字号 */
static uint16_t       s_row_y[UI_ROW_BUF];
static char           s_line_txt[UI_ROW_BUF][UI_LINE_MAX + 1];
static uint16_t       s_line_col[UI_ROW_BUF];
static uint8_t        s_line_valid[UI_ROW_BUF];
static uint8_t        s_line_len[UI_ROW_BUF];   /* ★ 上一帧这一行**实际画到**的字符数（含补的空格）*/
static uint8_t        s_full_redraw = 1;      /* 1 = 下一次要整屏重画 */
static uint16_t       s_bg          = WHITE;  /* 当前底色；换底色必须整屏重画 */
static uint16_t       s_view_seq      = 0xFFFF;   /* 上次渲染的快照序号 */
static uint8_t        s_view_unreliable;          /* 1 = 读到撕裂帧，画面顶部标 "!" */

static void (*s_unlock_hook)(void);             /* 长按解除锁定的出口（D5 由 app_protect 注册） */
static void (*s_ack_hook)(void);                /* 短按清除本地告警的出口 */

/* 按键状态机：只认电平，绝不 delay */
typedef struct
{
    uint8_t  lvl;           /* 已确认电平：1 = 松开 */
    uint8_t  cand;          /* 候选电平计数用 */
    uint8_t  cnt;
    uint8_t  pressed;       /* 当前是否处于"已确认按下" */
    uint32_t down_ms;
    uint8_t  long_fired;    /* 本次按下已经触发过长按 ⇒ 松手时不再算短按 */
} key_sm_t;

static key_sm_t s_key0;     /* KEY0：消音 / 长按解除锁定 */
static key_sm_t s_key1;     /* KEY1：翻页 */

/* ============================================================================
 *  版式与低层绘制
 * ==========================================================================*/

/**
 * @brief       由屏幕尺寸挑正文最大的字号，并算好各行 y 坐标与列宽
 * @note        需要放下的总行数 = 1 标题 + UI_BODY_ROWS 正文 + 1 页脚。
 *              正点原子字库只有 12/16/24/32 四档，从大到小试。
 */
static void ui_layout(void)
{
    static const uint8_t tbl[4] = { 32, 24, 16, 12 };
    uint16_t h = lcddev.height;
    uint8_t  i;
    uint8_t  f;
    uint8_t  tf;
    uint16_t need;

    s_font       = 12;
    s_title_font = 16;

    for (i = 0u; i < (uint8_t)(sizeof(tbl) / sizeof(tbl[0])); i++)
    {
        f  = tbl[i];
        tf = (uint8_t)((f <= 12u) ? 16u : ((f <= 16u) ? 24u : 32u));

        /* 标题 + 正文 + 页脚，全部含行间距 */
        need = (uint16_t)(UI_X + tf + UI_GAP);
        need = (uint16_t)(need + (UI_BODY_ROWS * (f + UI_GAP)));
        need = (uint16_t)(need + f + UI_X);

        if (need <= h && f <= APP_UI_FONT_DATA)
        {
            s_font       = f;
            s_title_font = tf;
            break;
        }
    }

    if (s_font > APP_UI_FONT_DATA)
    {
        s_font = APP_UI_FONT_DATA;
    }

    if (s_title_font > APP_UI_FONT_TITLE)
    {
        s_title_font = APP_UI_FONT_TITLE;
    }

    s_row_y[0] = UI_X;

    for (i = 1u; i < UI_ROW_BUF; i++)
    {
        uint8_t prev = (i == 1u) ? s_title_font : s_font;

        s_row_y[i] = (uint16_t)(s_row_y[i - 1] + prev + UI_GAP);
    }

    memset(s_line_valid, 0, sizeof(s_line_valid));
}

/**
 * @brief       画/更新一行（内容不变则完全不碰 LCD）
 * @param       row    0 = 标题行（用大字号），其余用正文字号
 * @param       color  文字颜色
 * @param       text   已格式化好的文本，超长截断、不足补空格以盖掉旧内容
 */
static void ui_row(uint8_t row, uint16_t color, const char *text)
{
    static char pad[UI_LINE_MAX + 1];
    uint8_t  font = (row == 0u) ? s_title_font : s_font;
    uint16_t len  = (uint16_t)strlen(text);
    uint8_t  cols = (uint8_t)((lcddev.width - 2u * UI_X) / (font / 2u));

    /* ★ 列宽按**本行字号**算：标题字号比正文大，若一律用正文列宽，
     *    一条刚好放得下的标题会越界换行、把下面几行的字盖掉。 */
    if (cols > UI_LINE_MAX)
    {
        cols = UI_LINE_MAX;
    }

    /* ★ 三级告警时整屏底色转红（听不见的人也要看得见），而**红字压红底等于看不见**
     *    ⇒ 在这种底色下把"告警色行"改黄、其余改白。只换颜色，不换判据、不换内容。 */
    if (s_bg == RED)
    {
        color = (color == RED || color == BRRED || color == YELLOW) ? YELLOW : WHITE;
    }

    if (row >= UI_ROW_BUF)
    {
        return;
    }

    if (len > cols)
    {
        len = cols;
    }

    memcpy(pad, text, len);

    /* ★ 只补到"上一帧这一行画到了第几列"，⛔ 不补满整行宽。
     *   原写法无条件补到 `cols`（正文一屏 40 列）⇒ 每行平均多写一倍纯背景像素。
     *   依据 = 实测：整页重绘 294ms 中逐行占 294ms、整屏清屏只占 26ms
     *   （`附录C` 走 A 的归因链；容器的判活窗口只有 80ms ⇒ 294ms 就是超窗本体）。
     *   残字擦除仍然完整：上一帧画到 `s_line_len[row]`，本帧就至少补到同一列。*/
    if (s_line_valid[row] != 0u && s_line_len[row] > (uint8_t)len)
    {
        while (len < s_line_len[row])
        {
            pad[len++] = ' ';
        }
    }

    pad[len] = '\0';

    if (s_line_valid[row] != 0u && s_line_col[row] == color && strcmp(s_line_txt[row], pad) == 0)
    {
        return;
    }

    memcpy(s_line_txt[row], pad, (uint16_t)(len + 1u));
    s_line_col[row]   = color;
    s_line_len[row]   = (uint8_t)len;
    s_line_valid[row] = 1u;

    lcd_show_string(UI_X, s_row_y[row], lcddev.width, font, font, pad, color);

    /* ★ 行界补心跳（走 A 的后半：把不可打断的最长一段压到"一行"这个粒度）。
     *   单行上限 ≈ 整页/行数 ≈ 30ms < 容器 80ms 窗口 ⇒ "确实在推进"的长重绘不再被误判成卡死，
     *   而"卡在某一行之内的真死"仍然会在不到 80ms 内被狗咬 —— ⛔ 补心跳的正当性只来自
     *   "这一段确有进展"，不许理解成"哪里慢就补哪里"（那等于把判活关掉）。
     *   ⚠️ 本行写的是**调用方那一 lane** 的槽：目前调用者只有容器 `task_legacy()`
     *      （占 `APP_TASK_UI` 槽）与它的泵 `app_poll()`，同一个槽 ⇒ 不会错配；
     *      D15 容器清空后本文件由 TaskUi 独占，槽号依旧对得上。*/
    app_tasks_heartbeat(APP_TASK_UI);
}

/**
 * @brief       清屏并作废所有行缓存（换页 / 底色变化时调用）
 */
static void ui_invalidate(uint16_t bg)
{
    lcd_clear(bg);
    s_bg = bg;
    memset(s_line_valid, 0, sizeof(s_line_valid));
}

/**
 * @brief       在标题行右侧固定位置打一个红 "!"：表示"这一屏是沿用上一帧，
 *              最近一次读取被写者打断（app_data_read 返回 2）"。
 * @note        ⛔ 不能静默沿用旧值 —— 那是本设计最反对的失效形态（§4.4）。
 *              它不进行缓存：这标记跨页存在、且不属于任何一页的正文。
 */
static void ui_mark_untrusted(void)
{
    uint16_t x = (uint16_t)(lcddev.width - UI_X - (s_title_font / 2u));

    lcd_show_string(x, s_row_y[0], s_title_font, s_title_font, s_title_font, "!",
                    (s_bg == RED) ? YELLOW : RED);
}

/* ==================== 数值格式化（全部定点，界面不参与任何浮点运算） ==================== */

/**
 * @brief       把 x10 量纲的有符号数写成 "12.3" / "-4.5"
 */
static void fmt_x10(char *dst, uint16_t dstsz, int16_t v)
{
    uint16_t a = (uint16_t)((v < 0) ? -v : v);

    snprintf(dst, dstsz, "%s%u.%u", (v < 0) ? "-" : "", (unsigned)(a / 10u), (unsigned)(a % 10u));
}

/**
 * @brief       单体电压：mV 写成 "3.301"；无效串写成 " ---- "
 * @note        ★ 无效必须是 0xFFFF(APP_MV_INVALID)。0mV 是**合法读数**（电芯被短接），
 *              绝不能与"没采到"共用一个显示，否则界面会把真实故障看成正常。
 */
static void fmt_cell(char *dst, uint16_t dstsz, uint16_t mv, uint8_t valid)
{
    if (valid == 0u || mv == APP_MV_INVALID)
    {
        snprintf(dst, dstsz, " ---- ");
        return;
    }

    snprintf(dst, dstsz, "%u.%03u", (unsigned)(mv / 1000u), (unsigned)(mv % 1000u));
}

/**
 * @brief       同一颗 `cell_mv[]` 的**整数 mV 列**（页 2 左列，对万用表时读这一列）
 * @note        ★ 与 `fmt_cell()` 是**同一个数的两种写法**，不是两个数据源：整数列给对表用
 *              （±1 mV 粒度、无取整歧义），小数列给人看。无效串的占位必须与小数列同款
 *              （" ----"），⛔ 不能印 0 —— 0mV 是合法读数。
 */
static void fmt_cell_int(char *dst, uint16_t dstsz, uint16_t mv, uint8_t valid)
{
    if (valid == 0u || mv == APP_MV_INVALID)
    {
        snprintf(dst, dstsz, "%5s", "----");
        return;
    }

    snprintf(dst, dstsz, "%5u", (unsigned)mv);
}

/**
 * @brief       温度：0.1℃ 写成 "25.3"；测点不可信时写 " --.- "
 * @note        与 `fmt_cell()` 同一条纪律（§4.4）—— ⛔ **0.0℃ 是合法读数**（冰水标定那一步就是 0），
 *              不能与"没采到"共用一个显示。环境通道在开机后的第一个节拍就是"没采到"
 *              （SHT30 是两阶段读法，首拍只发命令），画 0.0℃ 等于报一次假低温。
 */
static void fmt_temp(char *dst, uint16_t dstsz, int16_t v, uint8_t valid)
{
    if (valid == 0u)
    {
        snprintf(dst, dstsz, " --.- ");
        return;
    }

    fmt_x10(dst, dstsz, v);
}

/**
 * @brief       故障位图 → 短标签串（Tech_Design.md §6.2 的 16 位）
 * @note        界面只要"看得懂是哪一类"，完整原因串在 reason 行里。
 */
static void fmt_faults(char *dst, uint16_t dstsz, uint16_t bits)
{
    static const char *nm[16] =
    {
        "CL-OV", "CL-UV", "PK-OV", "PK-UV", "I-CHG", "I-DSG", "SHORT", "CUR-BAD",
        "T+CHG", "T-CHG", "T+DSG", "T-DSG", "T-DIF", "T-RATE", "V-DIF", "LINK"
    };
    uint8_t  i;
    uint16_t used = 0;

    dst[0] = '\0';

    if (bits == 0u)
    {
        snprintf(dst, dstsz, "none");
        return;
    }

    for (i = 0u; i < 16u; i++)
    {
        uint16_t l;
        uint16_t sep;

        if ((bits & (uint16_t)(1u << i)) == 0u)
        {
            continue;
        }

        l   = (uint16_t)strlen(nm[i]);
        sep = (used == 0u) ? 0u : 1u;      /* 分隔符只加在第二个及之后的标签前，避免尾部悬一个 '+' */

        if ((uint16_t)(used + sep + l + 1u) >= dstsz)
        {
            break;                          /* 放不下就截断，⛔ 不许越界写 */
        }

        if (sep != 0u)
        {
            dst[used++] = '+';
        }

        memcpy(dst + used, nm[i], l);
        used = (uint16_t)(used + l);
    }

    dst[used] = '\0';
}

/**
 * @brief       链路健康位图 → "OK" 或缺哪些通道
 */
static void fmt_link(char *dst, uint16_t dstsz, uint8_t health)
{
    static const struct
    {
        uint8_t     bit;
        const char *name;
    } tbl[6] =
    {
        { BMS_LINK_AFE,        "AFE"    },
        { BMS_LINK_INA226,     "INA"    },
        { BMS_LINK_NTC,        "NTC"    },
        { BMS_LINK_SHT30,      "SHT"    },
        { BMS_LINK_VSW,        "VSW"    },
        { BMS_LINK_FLASH_BUSY, "FLASH"  },
    };
    uint8_t  i;
    uint16_t used = 0;

    dst[0] = '\0';

    if (health == 0u)
    {
        snprintf(dst, dstsz, "all OK");
        return;
    }

    for (i = 0u; i < 6u; i++)
    {
        uint16_t l;

        if ((health & tbl[i].bit) == 0u)
        {
            continue;
        }

        l = (uint16_t)strlen(tbl[i].name);

        if ((uint16_t)(used + l + 2u) >= dstsz)
        {
            break;
        }

        memcpy(dst + used, tbl[i].name, l);
        used = (uint16_t)(used + l);
        dst[used++] = ' ';
    }

    dst[used] = '\0';
}

/* ============================================================================
 *  三个页面
 * ==========================================================================*/

/**
 * @brief       页 1 总况（FR-HMI-01）
 * @note        等级颜色只按快照里的 level 上色，⛔ 不在这里判门限。
 *              "数据新不新鲜"用 §4.4 的 `acq_is_fresh()` 判，它要的是当前毫秒，
 *              所以本函数收 now_ms —— 不收 now_ms 就只能永远显示 "live"，
 *              那是一条**假的安全感**（采集中断后界面还在动）。
 */
static void page_overview(const bms_snapshot_t *s, uint32_t now_ms)
{
    char line[UI_LINE_MAX + 1];
    char f1[20];
    char f2[20];
    uint32_t a;                         /* 电流绝对值：int16 取负会溢出，先升 u32 再排版 */
    uint8_t  fresh;
    uint16_t col;
    uint16_t tv;                        /* temp_valid 位图（两个测点各看各的） */

    fresh = (uint8_t)acq_is_fresh(s->raw.tick_ms, now_ms);

    /* ★ 串数与化学体系都不许写死字面量（G6/S14）：串数取 `APP_CELL_COUNT`，
     *    化学体系取快照里发布出来的 `der.chem` —— 换电池类型时界面跟着变，
     *    ⛔ 不留"改了宏标题还在说 4S LFP"这种会说谎的第二处。 */
    snprintf(line, sizeof(line), "BMS %uS %s   %s%s",
             (unsigned)APP_CELL_COUNT,
             (s->der.chem == CHEM_NCM) ? "NCM"
             : ((s->der.chem == CHEM_NA_ION) ? "Na" : "LFP"),
             (s->alarm.request_latch != 0u) ? "LATCHED  " : "",
             (fresh == 0u) ? "STALE" : "live");
    ui_row(0, (s->alarm.level >= BMS_LEVEL_L3) ? RED : BLUE, line);

    /* 两条组压通道并排显示：主通道(AFE) 与冗余通道(INA226) 差得远本身就是故障线索。
     * 量纲 10mV ⇒ 1321 = 13.21V，这里只插小数点，⛔ 不做任何阈值比较。 */
    snprintf(line, sizeof(line), "Pack  %u.%02u V   aux %u.%02u V",
             (unsigned)(s->raw.pack_10mv / 100u), (unsigned)(s->raw.pack_10mv % 100u),
             (unsigned)(s->raw.pack_10mv_aux / 100u), (unsigned)(s->raw.pack_10mv_aux % 100u));
    ui_row(1, (s->raw.pack_10mv == 0u) ? RED : BLUE, line);

    /* 电流符号必须显式打出来（充正放负）—— 现场最容易搞错的就是"到底在充还是在放" */
    a = (s->raw.curr_10ma < 0) ? (uint32_t)(-(int32_t)s->raw.curr_10ma) : (uint32_t)s->raw.curr_10ma;
    snprintf(line, sizeof(line), "Curr  %c%u.%02u A%s   SOC %u.%u%%",
             (s->raw.curr_10ma < 0) ? '-' : '+',
             (unsigned)(a / 100u), (unsigned)(a % 100u),
             (s->raw.curr_valid == 0u) ? " (!)" : "",
             (unsigned)(s->der.soc_x10 / 10u), (unsigned)(s->der.soc_x10 % 10u));
    ui_row(2, (s->raw.curr_valid == 0u) ? RED : BLUE, line);

    /* 两个测点各看各的 valid 位：环境通道归 app_acq（D3），电芯表面 NTC 到 D11 才有真实源
     * ⇒ 今天 cell 一律画 "--.-"（D2 的假温度已随 C-98 删掉，界面这一行**一行都不用改**就能看到差异） */
    tv = s->raw.temp_valid;
    fmt_temp(f1, sizeof(f1), s->raw.temp_x10[APP_TEMP_IDX_CELL_NTC],
             (uint8_t)((tv >> APP_TEMP_IDX_CELL_NTC) & 1u));
    fmt_temp(f2, sizeof(f2), s->raw.temp_x10[APP_TEMP_IDX_ENV],
             (uint8_t)((tv >> APP_TEMP_IDX_ENV) & 1u));
    snprintf(line, sizeof(line), "Temp  cell %s / amb %s C   SOH %u.%u%%",
             f1, f2,
             (unsigned)(s->der.soh_x10 / 10u), (unsigned)(s->der.soh_x10 % 10u));
    /* ★ 红色条件是"任一发红的通道来自 SHT30/NTC 链路不新鲜"，即：任一测点不可信就标红 */
    ui_row(3, ((tv & (uint16_t)((1u << APP_TEMP_IDX_CELL_NTC) | (1u << APP_TEMP_IDX_ENV))) ==
               (uint16_t)((1u << APP_TEMP_IDX_CELL_NTC) | (1u << APP_TEMP_IDX_ENV))) ? BLUE : RED, line);

    /* 等级 + 降额：这两行是"软件层现在要求什么"，全部抄快照，界面不自行推断（§4.2） */
    col = (s->alarm.level == BMS_LEVEL_L1) ? YELLOW
          : (s->alarm.level == BMS_LEVEL_L2) ? BRRED
          : (s->alarm.level >= BMS_LEVEL_L3) ? RED : BLUE;
    snprintf(line, sizeof(line), "Level %u   derate chg %u%% dis %u%%",
             (unsigned)s->alarm.level, (unsigned)s->alarm.derate_chg_pct,
             (unsigned)s->alarm.derate_dis_pct);
    ui_row(4, col, line);

    /* ★ 硬件保护层三态显示（审查 C-98）：AFE 还没接入时 `hw_layer_state` 恒为初值，
     *   旧的布尔写法 `!=0 ? ACTED : idle` 会把它显示成 "idle" —— 读起来像"硬件层已武装待动"，
     *   实际是**没有任何观测**。"不可知"必须有字面出口，⛔ 不得借用"待机"表达。
     *   红色条件收紧为"无故障 **且** 硬件层确实被观测到未动作"，其余一律红。 */
    fmt_faults(f1, sizeof(f1), s->alarm.fault_bits);
    snprintf(line, sizeof(line), "Fault %-12s HW %s",
             f1, (s->alarm.hw_layer_state == BMS_HW_ACTED) ? "ACTED"
             : ((s->alarm.hw_layer_state == BMS_HW_IDLE) ? "idle" : "unkn"));
    ui_row(5, ((s->alarm.fault_bits != 0u)
            || (s->alarm.hw_layer_state != BMS_HW_IDLE)) ? RED : GRAY, line);

    /* reason 是告警层给的原文（§4.4 随快照整体发布），界面只搬运不加工 ——
     * "不允许只报警不说明原因"（§5.3 要点 4）落到界面上就是这一行。 */
    snprintf(line, sizeof(line), "Why   %s",
             (s->alarm.reason[0] != '\0') ? s->alarm.reason : "--");
    ui_row(6, (s->alarm.level >= BMS_LEVEL_L2) ? RED : GRAY, line);

    fmt_link(f1, sizeof(f1), s->alarm.link_health);
    snprintf(line, sizeof(line), "Link  %s", f1);
    /* 除"环境温度缺失"外的任何通道缺口都会削弱保护判据 ⇒ 红（§5.2 失效安全映射表） */
    ui_row(7, ((s->alarm.link_health & (uint8_t)(~BMS_LINK_SHT30)) != 0u) ? RED : GRAY, line);

    /* 页脚：按键语义常驻 —— 现场只有两个人，其中一个还是第一次碰这块板 */
    if (s->alarm.request_latch != 0u)
    {
        snprintf(line, sizeof(line), "K1=page  HOLD K0 %us to unlock",
                 (unsigned)(APP_CTR_LATCH_HOLD_MS / 1000u));
    }
    else
    {
        snprintf(line, sizeof(line), "K1=page   K0=mute / ack");
    }

    ui_row(8, GRAY, line);
}

/**
 * @brief       页 2 单体电压（FR-HMI-01：4 串 + 压差）
 */
static void page_cells(const bms_snapshot_t *s)
{
    char line[UI_LINE_MAX + 1];
    char mv[8];
    char mvi[8];
    uint8_t i;
    uint8_t row = 1u;

    snprintf(line, sizeof(line), "Cell   mV  V     (of %u)", (unsigned)APP_CELL_COUNT);
    ui_row(0, BLUE, line);

    /* 逐串最多占 UI_BODY_ROWS-2 行：给下面的 Max/Min/Delta 与 Valid 两行**留位**，
     *  否则串数一多（本工程上限 16）汇总行就被挤到版式之外。 */
    for (i = 0u; i < (uint8_t)APP_CELL_COUNT && row <= (uint8_t)(UI_BODY_ROWS - 2u); i++)
    {
        uint8_t v = (uint8_t)((s->raw.cell_valid >> i) & 1u);

        fmt_cell(mv, sizeof(mv), s->raw.cell_mv[i], v);
        fmt_cell_int(mvi, sizeof(mvi), s->raw.cell_mv[i], v);
        snprintf(line, sizeof(line), "#%-3u%s  %s  %s",
                 (unsigned)(i + 1u), mvi, mv,
                 (v == 0u) ? "INVALID" : "");
        ui_row(row, (v == 0u) ? RED : BLUE, line);
        row++;
    }

    snprintf(line, sizeof(line), "Max %u  Min %u  Delta %u mV",
             (unsigned)s->der.cell_max_mv, (unsigned)s->der.cell_min_mv,
             (unsigned)s->der.delta_mv);
    /* ★ 这一行不上色是**有意的**：压差是否算故障由告警层判（页 1 的 Level/Fault 两行），
     *    界面这边一旦去比较 `APP_BMS_CELL_DIF_MV` 就越过了 §4.2 的职责线，
     *    而且那个门限当前是 0（Q12 未定数值）⇒ "非 0 即黄"会把正常压差染成告警色。 */
    ui_row(row, GRAY, line);
    row++;

    snprintf(line, sizeof(line), "Valid 0x%04X of 0x%04X",
             (unsigned)s->raw.cell_valid,
             (unsigned)(uint16_t)((APP_CELL_COUNT >= 16u) ? 0xFFFFu
                             : ((1u << APP_CELL_COUNT) - 1u)));
    ui_row(row, GRAY, line);
}

/**
 * @brief       页 3 第 1 行：判活取证（⛔ 不是判据，只把判活时已经算过的量印出来）
 * @note        格式 `Stall 0x位图 U峰值/窗口 P.. A.. C.. L..`，单位 ms，年龄上限显示 9999。
 *              下标顺序 = `app_task_id_t`（P=Protect A=Acq C=Comm L=Log U=迁移容器）。
 *              ★ `max` 是**历次判活时刻的峰值**，不是瞬时值：上一版印瞬时值，实测拍到的是
 *              `Stall 0x00`（一切正常）而同一分钟里日志已经投了 6 条 TASK-BAD ——
 *              容器一阵一阵超，200ms 采样几乎必然错过。峰值才留得住证据。
 *              ⚠️ 起点 = 本次上电，跨上电比较无意义（与注③同理）。
 *              ⏳ 与页 3 的调试行一起删除（D9 串口命令入口建好后由串口承担）。
 */
static void ui_alive_row(char *line, size_t size)
{
    uint32_t p = app_tasks_worst_age_ms(APP_TASK_PROTECT);
    uint32_t a = app_tasks_worst_age_ms(APP_TASK_ACQ);
    uint32_t c = app_tasks_worst_age_ms(APP_TASK_COMM);
    uint32_t l = app_tasks_worst_age_ms(APP_TASK_LOG);
    uint32_t u = app_tasks_worst_age_ms(APP_TASK_UI);

    /* 夹到 9999：tick 回绕那天差值是个巨大数，不夹会撑破定死的行宽 */
    if (p > 9999u) { p = 9999u; }
    if (a > 9999u) { a = 9999u; }
    if (c > 9999u) { c = 9999u; }
    if (l > 9999u) { l = 9999u; }
    if (u > 9999u) { u = 9999u; }

    /* ★ 排版是被实测逼出来的：上一版把 `U` 放在最后，57 字符超出屏幕可容纳宽度，
     *   被右沿切掉的正好就是要看的那一路 ⇒ 等于白拍一张照片。
     *   现在 `U`（唯一超窗的那一路）紧跟位图排在第 12~21 字符处，尾部四路是陪衬，
     *   真被切也不影响判读。`U` 带窗口，其余三条窗口 = 40/400/40/200（见 `app_tasks.c`）。 */
    snprintf(line, size, "Stall 0x%02X  U%u/%u  P%u A%u C%u L%u",
             (unsigned)app_tasks_stalled_last(),
             (unsigned)u, (unsigned)app_tasks_deadline_ms(APP_TASK_UI),
             (unsigned)p, (unsigned)a, (unsigned)c, (unsigned)l);

    ui_row(1, GRAY, line);
}

/**
 * @brief       页 3 事件（FR-HMI-01：最近 N 条带时间戳，最新在前）
 * @note        ★ D4 起数据源换成 BMS 事件日志（`app_log`，28 页环形追加 + 单条 CRC16）。
 *              三条显示口径是**故意**的，改之前先读：
 *              ① 只画**已落盘**的（`app_log_count()`）：待写队列里那几条还没进 Flash，掉电就没有，
 *                 把它们画出来会让人以为"已经存住了"。队列深度单独用 `pend=` 报出来。
 *              ② 第 0 行（最新一条）用红色 —— 这里的红不是"故障"，是"先看这行"。
 *              ③ 时间戳来自 RTC：**没装 VBAT 纽扣电池时每次上电都从固件编译时间重算** ⇒
 *                 跨上电比较时间戳没有意义，只有同一次开机内的先后关系可信。
 *              ④ `bad=` 是上电扫描出的 CRC 坏条数（判据②的界面出口）；它不为 0 时**不代表**
 *                 日志不可用 —— 一条坏只丢那一条，其余照样读得出来。
 *              ⑤ ★ 第 1 行的 `Stall` 是**取证出口**，不是判据：位图只说"谁超了"，
 *                 `U/P/A/C/L` 后面是"该任务在判活时刻见过的最大年龄"（单位 ms，`U` 另带它的
 *                 窗口）。区分的是两种完全不同的病：**刚好超一点**（节拍抖动，余量不足）与
 *                 **超一个数量级**（真卡死，某段不泵钩子的等待）。详见 `ui_alive_row()`。
 *                 ⏳ 随 D9 的串口命令入口一起删除。
 */

/**
 * @brief       ★★ 启动自检的**屏上出口**（页 3 的 row 1）
 * @note        为什么有它：本机**没有 COM 通路**（CH340 未枚举）⇒ 7 个启动自检的判定
 *              原先**只有调试器**能读。而 D6 到货当天人就在板子前面 ⇒ **能直接看到**才叫“照做即可”。
 *              顺序 = `app_main.c` 的 `APP_SELF_*`：**CFG / ALM / SOC / INA226 / AT24 / MODBUS / RS485**。
 *              `X` = 还没跑过（初值 `0xFF`）；数字 = 该模块的自检返回值（0 = 过）。
 *              ⛔ 本行**只读**，不参与任何判据（界面不做保护判断，§4.2）。
 */
static void ui_selfcheck_row(char *line, size_t size)
{
    char    d[8];
    uint8_t i;

    for (i = 0u; i < 7u; i++)
    {
        uint8_t rc = app_selfcheck_rc(i);

        d[i] = (rc == 0xFFu) ? 'X' : (char)('0' + (rc % 10u));
    }

    d[7] = '\0';

    /* ★★ 2026-09-29 扩（D6~D11 的快循环）：同一行右边再接**实时通道状态** `IO a/i/n`
     *   —— a = AFE、i = INA226、n = NTC，`1` = **该通道曾经成功读到过**（`*_ok > 0`）。
     *   ★ 为什么和自检判决同一行：两者回答的是同一个问题 —— "这些**接口**到底通没通"。
     *     自检是**上电那一刻**的判决，`IO` 是**此刻**的状态 ⇒ 到货当天插上线、
     *     等一拍（100ms）就能看见对应的那一位由 0 变 1，⛔ 不必开调试器。
     *   ⛔ 本行只读，不参与任何判据（§4.2：界面不做保护判断）。 */
    {
        uint16_t a_try = 0u, a_ok = 0u;
        uint16_t i_try = 0u, i_ok = 0u;
        uint16_t n_try = 0u, n_ok = 0u;
        char     io[8];

        app_acq_afe_stat(&a_try, &a_ok);
        app_acq_ina_stat(&i_try, &i_ok);
        app_acq_ntc_stat(&n_try, &n_ok);

        io[0] = (a_ok > 0u) ? '1' : '0';
        io[1] = '/';
        io[2] = (i_ok > 0u) ? '1' : '0';
        io[3] = '/';
        io[4] = (n_ok > 0u) ? '1' : '0';
        io[5] = '\0';

        (void)snprintf(line, size, "Self %c/%c/%c/%c/%c/%c/%c  IO %s",
                       d[0], d[1], d[2], d[3], d[4], d[5], d[6], io);
    }
}

static void page_events(void)
{
    char line[UI_LINE_MAX + 1];

#if APP_ENABLE_LOG
    uint16_t total = app_log_count();
    uint16_t shown;
    uint16_t i;

    snprintf(line, sizeof(line), "Events %u/%u  bad=%u pend=%u",
             (unsigned)total, (unsigned)APP_LOG_CAPACITY,
             (unsigned)app_log_damaged(), (unsigned)app_log_pending());
    ui_row(0, BLUE, line);

    /* ★ 启动自检行（row 1 原本空着）：七个自检的判定直接上屏，⛔ 不需要任何上位机/串口 */
    ui_selfcheck_row(line, sizeof(line));

    {
        uint8_t k;
        uint8_t all_ok = 1u;

        for (k = 0u; k < 7u; k++)
        {
            uint8_t rc = app_selfcheck_rc(k);

            if ((rc != 0u) && (rc != 0xFFu)) { all_ok = 0u; }   /* 0xFF = 还没跑，不当失败 */
        }

        ui_row(1, (all_ok != 0u) ? GREEN : RED, line);
    }

    ui_alive_row(line, sizeof(line));

    if (total == 0u)
    {
        ui_row(2, GRAY, "  (no event stored yet)");
        return;
    }

    shown = (uint16_t)((total > (uint16_t)(UI_BODY_ROWS - 2))
                       ? (uint16_t)(UI_BODY_ROWS - 2) : total);

    for (i = 0u; i < shown; i++)
    {
        app_log_rec_t rec;
        char          ts[16];
        char          ev[12];

        /* idx < total ⇒ 取不到只可能是这一条本身坏了（越界已由上面的 shown 挡住） */
        if (app_log_get(i, &rec) == 0u)
        {
            snprintf(line, sizeof(line), "  #%-3u <corrupt>", (unsigned)i);
            ui_row((uint8_t)(2u + i), RED, line);
            continue;
        }

        app_log_ts_str(rec.ts_unix, ts, sizeof(ts));
        app_log_event_str(rec.event, ev, sizeof(ev));

        snprintf(line, sizeof(line), "%s  %-9s 0x%04X  a=%u b=%u",
                 ts, ev, (unsigned)rec.code, (unsigned)rec.aux_a, (unsigned)rec.aux_b);
        ui_row((uint8_t)(2u + i), (i == 0u) ? RED : GRAY, line);
    }

#else
    ui_row(0, BLUE, "Events  (log disabled)");
    ui_alive_row(line, sizeof(line));
    ui_row(2, GRAY, "APP_ENABLE_LOG = 0");
#endif
}

/**
 * @brief       按当前页重绘
 */
static void ui_render(const bms_snapshot_t *s, uint32_t now_ms)
{
    uint16_t bg = (s->alarm.level >= BMS_LEVEL_L3) ? RED : WHITE;

    /* 三级告警时底色转红做视觉报警（听不见的人也要看得见）；其余一律白底。
     * 底色一换，整屏必须重画：逐行补丁会把白底上的旧字留在红底上。 */
    if (bg != s_bg)
    {
        s_full_redraw = 1u;
    }

    if (s_full_redraw != 0u)
    {
        ui_invalidate(bg);
        s_full_redraw = 0u;
    }

    switch (s_page)
    {
        case UI_PAGE_CELLS:  page_cells(s);  break;
        case UI_PAGE_EVENTS: page_events();  break;
        case UI_PAGE_OVERVIEW:
        default:             page_overview(s, now_ms); break;
    }
}

/* ============================================================================
 *  按键：只认电平、不阻塞，短按/长按分开
 * ==========================================================================*/
static void key_sm_init(key_sm_t *k)
{
    k->lvl        = 1u;
    k->cand       = 1u;
    k->cnt        = 0u;
    k->pressed    = 0u;
    k->down_ms    = 0u;
    k->long_fired = 0u;
}

/**
 * @brief       推进一步状态机
 * @param       lvl_now  1 = 松开, 0 = 按下（读 GPIO 的结果）
 * @retval      0=无事件 1=短按(已松手) 2=长按(按住到点即触发)
 */
static uint8_t key_sm_step(key_sm_t *k, uint8_t lvl_now, uint32_t now_ms)
{
    if (lvl_now != k->lvl)
    {
        if (lvl_now != k->cand)
        {
            k->cand = lvl_now;
            k->cnt  = 1u;
            return 0;
        }

        if (++k->cnt < UI_KEY_DEBOUNCE_N)
        {
            return 0;
        }

        k->lvl = lvl_now;
        k->cnt = 0u;

        if (lvl_now == 0u)                      /* 确认按下 */
        {
            k->pressed    = 1u;
            k->down_ms    = now_ms;
            k->long_fired = 0u;
            return 0;
        }

        if (k->pressed != 0u && k->long_fired == 0u)
        {
            k->pressed = 0u;
            return 1;                           /* 松手且没到长按时长 ⇒ 短按 */
        }

        k->pressed = 0u;
    }
    else
    {
        /* ★ 采样电平与已确认电平一致 ⇒ 抖动计数必须作废。缺这一步时 `cand/cnt` 会跨事件残留：
         *   两次相隔很远、各只有 1 个采样的毛刺就能凑满 `UI_KEY_DEBOUNCE_N`，
         *   消抖实际退化成"一次采样即确认"。 */
        k->cand = lvl_now;
        k->cnt  = 0u;

        if (k->pressed != 0u && k->long_fired == 0u)
        {
            /* 无符号回绕安全：不要写成 now > down + HOLD */
            if ((uint32_t)(now_ms - k->down_ms) >= (uint32_t)APP_CTR_LATCH_HOLD_MS)
            {
                k->long_fired = 1u;
                return 2;                       /* 长按：按住到点就触发，不等松手 */
            }
        }
    }

    return 0;
}

/**
 * @brief       短按 KEY0 = 清除本地告警（等价 Modbus 线圈 0x0003）
 * @note        ⛔ 解除不了三级锁定，那是长按的专属动作（S11）。
 *              顺序：先交给注册者(app_protect/D14)，没有注册者时退化为"让蜂鸣器消音" ——
 *              蜂鸣器是两侧共用的输出通道（AGENTS §2.1），消音不算保护判断。
 */
static void ui_do_ack(void)
{
    if (s_ack_hook != NULL)
    {
        s_ack_hook();
        printf("UI   : KEY0 ack -> alarm layer\r\n");
        return;
    }

    if (alarm_key_mute() != 0u)
    {
        printf("UI   : KEY0 mute (no owner registered yet)\r\n");
    }
    else
    {
        printf("UI   : KEY0 ack (nothing to mute)\r\n");
    }
}

/**
 * @brief       长按 KEY0 5s = 请求解除三级锁定（S11）
 * @note        界面**只发请求**：没有注册者时明确回显"被忽略"，
 *              ⛔ 绝不自己清 request_latch（那会绕过 app_protect，违反 S9）。
 */
static void ui_do_unlock_request(void)
{
    if (s_unlock_hook != NULL)
    {
        s_unlock_hook();
        printf("UI   : KEY0 held %us -> unlock request sent to app_protect\r\n",
               (unsigned)(APP_CTR_LATCH_HOLD_MS / 1000u));
        return;
    }

    printf("UI   : KEY0 held %us -> unlock request IGNORED (app_protect not wired yet, D5)\r\n",
           (unsigned)(APP_CTR_LATCH_HOLD_MS / 1000u));
}

/* ============================================================================
 *  对外接口
 * ==========================================================================*/
void app_ui_set_unlock_hook(void (*hook)(void))
{
    s_unlock_hook = hook;
}

void app_ui_set_ack_hook(void (*hook)(void))
{
    s_ack_hook = hook;
}

/**
 * @brief       ★ 从协议侧触发一次"短按清除本地告警"（Modbus 线圈 0x0003）
 * @retval      1 = 请求已交给 hook（或 hook 未注册时的静音兜底路径）
 * @note        为什么必须有这个入口，而不是让 `app_modbus.c` 直接去抓一个静音函数：
 *              "短按 KEY0 该做什么"的**唯一属主是这里**（`s_ack_hook` 的注册点 +
 *              下面 `ui_do_ack()` 的兜底语义）。协议层另抓一个函数 = 第二条通路，
 *              两条通路今天恰好等价、明天一定不等价 —— 而"线圈点下去没反应"与
 *              "线圈点下去反应错了"在总线上看到的是同一个字节（C-97 F1 同族）。
 *              ⛔ 本函数**不解除三级锁定**（那是长按 5s 的专属动作，S11）。
 */
uint8_t app_ui_request_ack(void)
{
    ui_do_ack();

    return 1u;
}

uint8_t app_ui_page(void)
{
    return s_page;
}

void app_ui_init(void)
{
    key_sm_init(&s_key0);
    key_sm_init(&s_key1);

    ui_layout();

    s_full_redraw = 1u;
    s_view_seq     = 0xFFFFu;               /* 强制第一次一定要画 */
    s_view_unreliable = 0u;
}

void app_ui_task(uint32_t now_ms)
{
    static uint32_t s_next_ms;
    static uint32_t s_key_ms;
    bms_snapshot_t  s;
    uint8_t         ev0;
    uint8_t         ev1;
    uint8_t         ret;

    /* ---- 1. 按键：绝不为刷屏让路，但采样要有节拍下限（见 UI_KEY_SCAN_MS 的说明） ---- */
    if ((uint32_t)(now_ms - s_key_ms) >= (uint32_t)UI_KEY_SCAN_MS)
    {
        s_key_ms = now_ms;
        ev0 = key_sm_step(&s_key0, (KEY0_READ() == GPIO_PIN_SET) ? 1u : 0u, now_ms);
        ev1 = key_sm_step(&s_key1, (KEY1_READ() == GPIO_PIN_SET) ? 1u : 0u, now_ms);
    }
    else
    {
        ev0 = 0u;
        ev1 = 0u;
    }

    if (ev0 == 2u)
    {
        ui_do_unlock_request();
    }
    else if (ev0 == 1u)
    {
        ui_do_ack();
    }

    if (ev1 == 1u)
    {
        s_page = (uint8_t)((s_page + 1u) % UI_PAGE_COUNT);
        s_full_redraw = 1u;                  /* 换页一定整屏重画，避免留下上一页的残字 */
        printf("UI   : page -> %u\r\n", (unsigned)s_page);
    }

    /* ---- 2. 刷屏限流：数据没换 + 没到点就完全不碰 FSMC ---- */
    if ((int32_t)(now_ms - s_next_ms) < 0)
    {
        if (s_full_redraw == 0u)
        {
            return;
        }
    }

    s_next_ms = now_ms + 200u;

    ret = app_data_read(&s);

    /* 撕裂帧(ret=2)：⛔ 这份内容不能当数据用（app_data.h 的接口约定），但也不能沉默 ——
     *  画面沿用上一帧 + 标题右侧打一个红 "!"，让"这一屏是旧的"这件事看得见。 */
    if (ret == 2u)
    {
        s_view_unreliable = 1u;
        ui_mark_untrusted();
        return;
    }

    /* 从未发布(ret=1)：字段还是上电的"全通道无效"初值，打数值会打出一堆 0，
     *  而 0V/0A 在界面上是**合法读数** ⇒ 必须打"没有数据"这四个字。 */
    if (ret == 1u)
    {
        if (s_full_redraw != 0u)
        {
            ui_invalidate(WHITE);
            s_full_redraw = 0u;
        }

        ui_row(0, RED, "BMS  no data yet");
        ui_row(1, GRAY, "K1=page   waiting for acquisition...");
        return;
    }

    /* 从"不可信"回到可信：那个 "!" 画在行缓存之外，只能靠整屏重画抹掉。 */
    if (s_view_unreliable != 0u)
    {
        s_view_unreliable = 0u;
        s_full_redraw     = 1u;
    }

    if (s_full_redraw == 0u && app_data_seq() == s_view_seq)
    {
        return;                              /* 同一帧 + 不是强制重画 ⇒ 什么都不做 */
    }

    s_view_seq = app_data_seq();
    ui_render(&s, now_ms);
}
