/**
 ****************************************************************************************************
 * @file        actuator_mos.c
 * @brief       执行器接口 `actuator.h` 的实现 —— 背靠背 N-MOS 共源低边开关 + 预充回路
 * @note        移植来源：本项目自写（无厂商参考代码；`Tech_Design.md` §3.2 的拓扑与 §5.1 的
 *              与逻辑真值表是唯一的依据）。
 *              改动点（相对 §4.5 的原始契约）：
 *                ① ★ **新增 `actuator_precharge_stop()`** —— 原契约只有 `start`，而 10Ω/10W 水泥
 *                   电阻长期串进主回路会烧（≈1.46A@14.6V ⇒ ≈21W > 额定 10W），
 *                   "预充忘了关"在软件里没有任何现象 ⇒ 必须有一个显式的关。（`附录C` C-113）
 *                ② 返回码由"0/1"改为 `ACT_OK/ACT_ERR_*`：五种失败形态的**处置不同**
 *                   （`CFG` 要查 CubeMX 配置、`AFE` 要查线束与后缀、`STATE` 是状态机写错），
 *                   合成一个"失败"就又要靠猜（同 C-107 的 f) 三列口径）。
 *                ③ `open()` 无条件连带关预充（理由见 `actuator.h` 的函数注）。
 *              验证方式：⚠️ **本次只到编译级 + 门禁级** —— 板上还没有 A 采样板与执行器
 *              （`Build.md` **D12** 判据①= 示波器看 `EN_x` → 栅极波形、判据② = 拔 MCU 供电主 MOS
 *              必须关断）。本文件里能被软件证明的只有"引脚配置与引脚电平"，
 *              ⛔ 不得据此声称"执行器已验证"。
 ****************************************************************************************************
 */

#include "./BSP/ACTUATOR/actuator.h"
#include "./BSP/AFE/afe.h"          /* 与逻辑的 AFE 侧：afe_set_channel()（S15：只走契约，不碰寄存器） */
#include <stdio.h>                  /* 仅 actuator_init() 的回读打印（调度器之前，单线程） */

/* ===================== 引脚（★ 本文件是唯一允许出现这三个引脚宏的地方，S15 / G7-①） =====================
 * 出处 = `Tech_Design.md` §2 引脚表：PE0=充电使能、PE1=放电使能、PE2=预充使能，
 * ★ 三个都必须**外接 10K 下拉**（S5 的三不可省元件之一）—— 那才是"MCU 死机/复位时也断开"的保证，
 *   本文件的任何代码都给不了这一条。 */
#define ACT_PORT        GPIOE
#define ACT_PIN_CHG     GPIO_PIN_0
#define ACT_PIN_DSG     GPIO_PIN_1
#define ACT_PIN_PRE     GPIO_PIN_2
#define ACT_PIN_ALL     (ACT_PIN_CHG | ACT_PIN_DSG | ACT_PIN_PRE)
#define ACT_IDX_CHG     0u          /* GPIOE->CRL 里 PE0 的 4 位域序号 */
#define ACT_IDX_DSG     1u
#define ACT_IDX_PRE     2u

/* 状态位图里执行器自己拥有的三位（其余位归别人，见 actuator.h 的位图注） */
#define ACT_ST_EN_MASK  (ACT_ST_CHG | ACT_ST_DSG)
#define ACT_ST_MAIN     (ACT_ST_CHG | ACT_ST_DSG)

/* ===================== 引脚读写的唯一通路 =====================
 * 三个函数而不是三处裸调用：`get_state()` 与状态机的对账必须读到**同一个东西**，
 * 写路径也只有一条，才不会出现"某一路绕过了回读校验"。 */

/** @brief 读三路使能引脚的**真实电平**（F1 输出模式下 IDR 仍反映引脚电平） */
static uint8_t act_en_read(void)
{
    uint8_t st = 0u;

    if (HAL_GPIO_ReadPin(ACT_PORT, ACT_PIN_CHG) == GPIO_PIN_SET)
    {
        st |= ACT_ST_CHG;
    }

    if (HAL_GPIO_ReadPin(ACT_PORT, ACT_PIN_DSG) == GPIO_PIN_SET)
    {
        st |= ACT_ST_DSG;
    }

    if (HAL_GPIO_ReadPin(ACT_PORT, ACT_PIN_PRE) == GPIO_PIN_SET)
    {
        st |= ACT_ST_PRE;
    }

    return st;
}

/** @brief 按位图驱动三路使能引脚 */
static void act_en_write(uint8_t st)
{
    HAL_GPIO_WritePin(ACT_PORT, ACT_PIN_CHG, (st & ACT_ST_CHG) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ACT_PORT, ACT_PIN_DSG, (st & ACT_ST_DSG) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(ACT_PORT, ACT_PIN_PRE, (st & ACT_ST_PRE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/** @brief 回读校验：要求 `set_mask` 那些位为高、`clr_mask` 那些位为低 */
static uint8_t act_en_verify(uint8_t set_mask, uint8_t clr_mask)
{
    uint8_t now = act_en_read();

    if (((now & set_mask) == set_mask) && ((now & clr_mask) == 0u))
    {
        return ACT_OK;
    }

    return ACT_ERR_PIN;
}

/** @brief 该引脚的 GPIOE->CRL 配置是不是"推挽输出且 MODE 非 0"（= 不是浮空输入、不是复用） */
static uint8_t act_pin_is_pp_output(uint8_t idx)
{
    uint32_t field = (ACT_PORT->CRL >> (4u * idx)) & 0xFu;
    uint32_t mode  = field & 0x3u;          /* MODE[1:0] */
    uint32_t cnf   = (field >> 2) & 0x3u;   /* CNF[1:0]：00=推挽输出，10=复用，11=输入 */

    return ((cnf == 0u) && (mode != 0u)) ? 1u : 0u;
}

/* ===================== 与逻辑：EN_x 与 AFE 通道位 =====================
 * §5.1 的真值表：`EN=0` 时 2N7002 导通，把主 MOS 栅极**短路到源极** ⇒ 主 MOS 强制关断，
 * 而且这一条与 AFE 侧的通道位是什么**无关**。⇒ 本文件因此得到两条简化：
 *   · 回退只需要把 `EN_x` 拉回低，不需要"再写一次 AFE 位并把失败重试"（重试反而要引入循环）；
 *   · 反过来 `EN=1` 单独不构成导通 —— 所以 AFE 侧写失败时，"断开"这件事仍然成立，
 *     状态机可以安心停在原状态等下一次（FR-PRT-05：不可知一律朝保守方向降级）。
 * `chg`/`dsg` 入参 = 期望的两路授权位（位图 `ACT_ST_EN_MASK`）。 */
static uint8_t act_apply(uint8_t en_bits)
{
    uint8_t prev = act_en_read();
    uint8_t keep_pre = (uint8_t)(prev & ACT_ST_PRE);
    uint8_t target = (uint8_t)((en_bits & ACT_ST_EN_MASK) | keep_pre);
    uint8_t pin_rc;
    uint8_t afe_rc;

    act_en_write(target);
    pin_rc = act_en_verify((uint8_t)(target & ACT_ST_EN_MASK),
                           (uint8_t)((uint8_t)ACT_ST_EN_MASK & (uint8_t)~target));

    if (pin_rc != ACT_OK)
    {
        /* 引脚没按意图变 = 驱动通路不可信（被别的外设接管、焊错、短路到轨），
         * 立刻退回调用前的状态。⛔ 不得"继续往下写 AFE"：那会留下一个
         * "我以为拉高了、其实不知道现在是几伏"的中间态。 */
        act_en_write(prev);
        return ACT_ERR_PIN;
    }

    /* ★ 顺序：先放行软件层（EN），再请求硬件层（AFE 通道位）。
     * 反序的话，中间那一拍是"AFE 已放行、EN 还低"= 仍然断开，无害；
     * 但失败回退时就要**先清 AFE 再清 EN** 才对称 —— 多一套分支，少一处可证。
     * 现在这个顺序下，任何一步失败的回退都只是"把 EN 写回去"。 */
    afe_rc = afe_set_channel((target & ACT_ST_CHG) ? 1 : 0, (target & ACT_ST_DSG) ? 1 : 0);

    if (afe_rc != AFE_OK)
    {
        act_en_write(prev);                 /* 回退 EN 就足够构成"断开"，理由见上面的真值表 */
        return ACT_ERR_AFE;
    }

    return ACT_OK;
}

/* ===================== 契约实现 ===================== */

uint8_t actuator_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t          rc   = ACT_OK;

    /* F1 的 HAL_GPIO_Init() 不代开 GPIO 时钟（`softi2c.c` 里踩过同一个坑，注释就写在那儿）。
     * 漏开时钟的症状 = 引脚毫无反应、也不报错。 */
    __HAL_RCC_GPIOE_CLK_ENABLE();

    /* ★ 先写 0 再切模式（S3 / FR-PRT-04 的代码表达）：F1 的 BSRR/ODR 在输入模式下也可写，
     * 所以这一句能保证"变成推挽输出的那一拍，输出级已经是低"，中间没有一瞬间的高。
     * ⚠️ 反序的话，`GPIO_Init()` 之后引脚立即驱动 ODR 的复位值 —— 复位值本来就是 0，
     *      所以两种写法今天都对；但"对"是因为运气（复位默认值），不是因为代码。
     *      写成显式顺序 = 下一个人改代码时不需要重新推一遍。 */
    ACT_PORT->BRR = (uint32_t)ACT_PIN_ALL;

    gpio.Pin   = ACT_PIN_ALL;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;         /* ★ 不开内部上/下拉：内部 40K 级电阻与外部 10K 下拉分压会抬高"关断电平"，
                                       *   而这三路的关断判据是外部下拉保证的（S5）。上拉尤其禁止 ——
                                       *   它会在 MCU 复位（GPIO 变浮空输入）期间把引脚顶到一个不确定的中间电平。 */
    gpio.Speed = GPIO_SPEED_FREQ_LOW; /* 2MHz 足够：这是使能信号，不是高速开关，慢一点栅极振铃小 */
    HAL_GPIO_Init(ACT_PORT, &gpio);

    /* ---------- 回读 1：配置位。★ 这里查的是"是不是推挽输出"，不是"是不是低" ----------
     * 因为本项目的已知风险是 **PE0/PE1 = FSMC_NBL0/NBL1**（`Tech_Design.md` §2 引脚表注）：
     * 若 CubeMX 勾选了字节使能，FSMC 会接管这两脚，空闲态输出**高** ⇒ `EN_x` 永久放行
     * = 软件层静默少一层（S1 的双层保护塌陷），而且 LCD 照常显示、不报任何错。
     * ⚠️ §4 自检清单里那条"判据是能拉低"是**人工**动作；本函数把它做成**每次开机都跑**的配置核查，
     *    两者不互相替代：本函数看不见"被拉高之后又被 FSMC 抢回去"，人工那条看得见。 */
    if ((act_pin_is_pp_output(ACT_IDX_CHG) == 0u) ||
        (act_pin_is_pp_output(ACT_IDX_DSG) == 0u) ||
        (act_pin_is_pp_output(ACT_IDX_PRE) == 0u))
    {
        rc = ACT_ERR_CFG;
    }

    /* ---------- 回读 2：真实电平必须是全低（常态断开，S3）---------- */
    if ((rc == ACT_OK) && (act_en_read() != 0u))
    {
        rc = ACT_ERR_PIN;
    }

    printf("ACT  : en_chg=%u en_dsg=%u en_pre=%u cfg=%s\r\n",
           (act_en_read() & ACT_ST_CHG) ? 1u : 0u,
           (act_en_read() & ACT_ST_DSG) ? 1u : 0u,
           (act_en_read() & ACT_ST_PRE) ? 1u : 0u,
           (rc == ACT_ERR_CFG) ? "NOT-OUTPUT-PPOUT(check FSMC NBL)" :
           (rc == ACT_ERR_PIN) ? "NOT-LOW" : "ok");

    return rc;
}

uint8_t actuator_close(bms_dir_t dir)
{
    uint8_t now = act_en_read();
    uint8_t target;

    switch (dir)
    {
        case ACT_DIR_CHG:  target = (uint8_t)(now | ACT_ST_CHG); break;
        case ACT_DIR_DSG:  target = (uint8_t)(now | ACT_ST_DSG); break;
        case ACT_DIR_BOTH: target = (uint8_t)(now | ACT_ST_MAIN); break;
        default:           return ACT_ERR_PARAM;
    }

    return act_apply(target);
}

uint8_t actuator_open(bms_dir_t dir)
{
    uint8_t now = act_en_read();
    uint8_t target;

    switch (dir)
    {
        case ACT_DIR_CHG:  target = (uint8_t)(now & (uint8_t)~ACT_ST_CHG); break;
        case ACT_DIR_DSG:  target = (uint8_t)(now & (uint8_t)~ACT_ST_DSG); break;
        case ACT_DIR_BOTH: target = (uint8_t)(now & (uint8_t)~ACT_ST_MAIN); break;
        default:           return ACT_ERR_PARAM;
    }

    /* ★ 无条件连带关预充（不区分 dir）：预充 MOS 与 10Ω 并联在主开关两端，
     * 主开关断开而预充导通时负载电流全程走 10Ω ⇒ ≈21W 打在 10W 电阻上。
     * 详见 actuator.h 的 open() 函数注。 */
    target = (uint8_t)(target & (uint8_t)~ACT_ST_PRE);

    return act_apply(target);
}

uint8_t actuator_precharge_start(void)
{
    uint8_t now = act_en_read();

    /* ★ 互锁：主开关已闭合时拒绝启动预充（见文件头改动点①的功率账）。
     * 正常时序里这一步不该发生 —— 状态机只在 SAFE_OFF→PRECHARGE 调它，而那时主开关必断；
     * 走到这里说明**状态机与引脚现状不一致**（比如上一次 close 之后被复位、或有人绕过属主写了引脚），
     * 那正是必须报错而不是"照做"的场景。 */
    if ((now & ACT_ST_MAIN) != 0u)
    {
        return ACT_ERR_STATE;
    }

    act_en_write((uint8_t)(now | ACT_ST_PRE));

    /* set = 预充必须已经高，clr = 主开关两路必须仍是低（互锁在上面，这里是**证据**）*/
    return act_en_verify(ACT_ST_PRE, ACT_ST_MAIN);
}

uint8_t actuator_precharge_stop(void)
{
    uint8_t now = act_en_read();

    act_en_write((uint8_t)(now & (uint8_t)~ACT_ST_PRE));

    /* 只校验预充已低：主开关那一路此刻应当是"刚闭合"（make-before-break，§5.2），
     * 把它的期望值写死在这里会让正常路径报假错。 */
    return act_en_verify(0u, ACT_ST_PRE);
}

uint8_t actuator_get_state(void)
{
    return act_en_read();
}
