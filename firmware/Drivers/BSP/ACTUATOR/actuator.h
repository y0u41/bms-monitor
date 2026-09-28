/**
 ****************************************************************************************************
 * @file        actuator.h
 * @brief       执行器（背靠背 N-MOS 共源低边开关 + 预充回路）对应用层的**唯一**接口
 * @note        1, 契约来源 = `Tech_Design.md` §4.5；拓扑、引脚、栅极驱动细节对本层以下不可见
 *                  （S15：`User/App` 目录下的每个 `.c` 都不得出现执行器引脚宏）。换执行器形态（接触器）
 *                  只加一个 `actuator_contactor.c`，应用层零改动（§4.6 决定 3）。
 *              2, ⛔ **只允许 `app_protect.c` 调用本接口**（S9 / `PRD.md` FR-PRT-10）。
 *                  该边界由 `Tools/gate_check.sh` 的 **G7-⑦** 机械守住（数调用点、限定文件域），
 *                  ⛔ 不写成头注里的纪律 —— 纪律不会自己守住（同 C-97 F1 / C-103③ 的教训）。
 *              3, ★ 本层刻意做"笨"实现：**不含时序、不含状态机、不含门限判断**。
 *                  它只有两处自己的判断，都是失效安全方向：
 *                    ① `close` 的任一步失败 ⇒ 立即把该路使能拉回低（不留在"半放行"状态）；
 *                    ② 主开关已闭合时拒绝启动预充（10Ω/10W 长期串进主回路 = ≈21W@14.6V，必烧）。
 *              4, ⛔ 禁止在中断里调用（S8）：`close`/`open` 内部要写 AFE 的通道使能位，
 *                  那是 `g_bus_afe` 上的一次阻塞式软件 I2C 事务（有界，见 `softi2c.c` 的 `WAIT_LIMIT`）。
 *              5, ★ **回读的是"引脚电平"，不是"我的意图"**：F1 的 GPIO 配成输出后 `IDR` 仍反映
 *                  引脚真实电平，所以 `actuator_get_state()` 答的是"板子上现在是多少伏"。
 *                  ⚠️ 但它**答不出**栅极那一段（2N7002 → 470Ω → 主 MOS 栅极）是否真的动 ——
 *                  那要示波器，排 `Build.md` D12 判据①。
 ****************************************************************************************************
 */

#ifndef __ACTUATOR_H
#define __ACTUATOR_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_config.h"

/* ===================== 方向（§4.5 的 `bms_dir_t`） =====================
 * 主开关是**背靠背**两只 N-MOS（共源），充电与放电各管一只，所以"导通/切断"必须分方向说。
 * ⛔ 不得简化成一个"总开关"位：那是把 S1 的双向独立切断能力写没了（充电 MOS 坏了
 *    仍可放电，反之亦然 —— 本设计的降级路径依赖这一点）。 */
typedef enum
{
    ACT_DIR_CHG = 0,        /* 充电通路 */
    ACT_DIR_DSG = 1,        /* 放电通路 */
    ACT_DIR_BOTH = 2        /* 两路一起（进 SAFE_OFF / FAULT_LATCHED 用这一档） */
} bms_dir_t;

/* ===================== 返回码（同 `afe.h` 的口径：不是 0/1） ===================== */
#define ACT_OK              0
#define ACT_ERR_AFE         1   /* AFE 侧通道使能写失败 ⇒ **该次 close 已回退到断开**（S1 与逻辑：任一层不放行 = 断） */
#define ACT_ERR_PIN         2   /* 使能引脚回读与意图不符 = 驱动通路本身不可信，已回退到断开 */
#define ACT_ERR_CFG         3   /* ★ GPIO 配置回读不是"推挽输出、非复用" —— FSMC 字节使能抢占 PE0/PE1 的现场
                                 *   （§2 引脚表注：被抢占时引脚**永久拉高 = 软件层永久放行**，
                                 *    而"能拉高"这种测法看不见它，所以这里查的是配置位而不是电平） */
#define ACT_ERR_STATE       4   /* 非法时序：主开关已闭合时启动预充（见上面第 ③ 条） */
#define ACT_ERR_PARAM       5   /* 入参非法（dir 不在枚举内） */

/* ===================== 状态位（`actuator_get_state()` 的返回值） =====================
 * ⚠️ 本层只报**自己拥有的**三位（三个使能引脚的真实电平）。
 *   `Tech_Design.md` §6.1 的 0x0020 那格还要求"锁定"与"AFE 硬件层已动作"两位 ——
 *   那两位的属主不是执行器（锁定属 `app_protect`，硬件层属 `afe_hw_fault()` 的解码），
 *   由本层一起报就等于让一个盒有两个写者（C-97 F1 的同族）。⇒ 由 `app_protect.c` 组装。 */
#define ACT_ST_CHG          0x01u   /* 充电使能引脚 = 高 */
#define ACT_ST_DSG          0x02u   /* 放电使能引脚 = 高 */
#define ACT_ST_PRE          0x04u   /* 预充使能引脚 = 高 */

/**
 * @brief       初始化三个使能引脚为**推挽输出、初值低**，并回读配置与电平
 * @retval      ACT_OK / ACT_ERR_CFG / ACT_ERR_PIN
 * @note        ★ 顺序要求：**先写 ODR=0，再切成输出**。F1 的 `GPIO_Init()` 不改 ODR，
 *              若先切模式后写 0，中间那一拍引脚已经在驱动"上一次的模式默认值"；
 *              本板这三个引脚外部有 10K 下拉（S5 三不可省元件之一），MCU 侧再保证
 *              "从浮空到驱动的整个窗口都输出低"才是真常态断开（S3 / FR-PRT-04）。
 * @note        ⛔ 必须在 `vTaskStartScheduler()` **之前**调用：调度器起来之后，
 *              `TaskProtect` 的第一拍就可能基于"还没初始化"的引脚做判断。
 * @note        本函数只在开机跑一次、由 `app_main()` 调用，所以它**可以**打印一行回读结果
 *              （单线程、无 `printf` 交错，口径同 `app_protect_wdg_start()`）。
 */
uint8_t actuator_init(void);

/**
 * @brief       导通某方向（或两路）：软件层使能 + AFE 侧通道使能，两层一起放行
 * @param       dir  ACT_DIR_CHG / ACT_DIR_DSG / ACT_DIR_BOTH
 * @retval      ACT_OK / ACT_ERR_AFE / ACT_ERR_PIN / ACT_ERR_PARAM
 * @note        ★ **与逻辑的落点**（S1 / FR-PRT-01）：主 MOS 的栅极由"AFE 驱动"与"EN_x 不短路"
 *              **同时**成立才导通。所以本函数做的是两件事：
 *                1) `EN_x` 拉高（= 2N7002 关断 = 不再把栅极短路到源极）；
 *                2) AFE 侧把对应通道位置 1。
 *              ⛔ 任一步失败都必须把第 1 步已经拉高的那一路**拉回低**再返回错误 ——
 *              留在"EN 高、AFE 不放行"的状态看似无害（还是断开的），但它会让下一次的
 *              失败归因变成两件候选混在一起，且一旦 AFE 侧随后自己放行就没有人拦得住。
 * @note        ⚠️ 本函数**不含"必须先预充"这条时序判据** —— 那是 `app_protect.c` 的职责（S9）。
 *              做成两层都判就会有两个时序真值；做成一层判就只有一个。
 */
uint8_t actuator_close(bms_dir_t dir);

/**
 * @brief       切断某方向（或两路）
 * @param       dir  ACT_DIR_CHG / ACT_DIR_DSG / ACT_DIR_BOTH
 * @retval      ACT_OK / ACT_ERR_AFE / ACT_ERR_PIN / ACT_ERR_PARAM
 * @note        ★ 无论传入哪个 `dir`，本函数都会**顺带把预充使能拉回低**。
 *              理由不是"顺手"，是这条通路的失效形态：预充 MOS 与 10Ω 是**并联在主开关两端**的，
 *              主开关一旦断开而预充还导通，负载电流就要长期走 10Ω（≈1.46A ⇒ ≈21W，
 *              电阻额定 10W）—— 而"预充忘了关"在软件里没有任何现象。
 *              ⛔ 不得改成"只有 `ACT_DIR_BOTH` 才关预充"：单方向切断（例如 0℃ 禁充把充电关掉）
 *              同样可能出现在预充窗口里。
 * @note        断开方向的 AFE 写失败**不回退**（与 `close` 相反）：这一侧的保守方向就是"断"，
 *              引脚已经先拉低了，回退反而会把通路又放行一次。失败仍要返回错误码让状态机记日志。
 */
uint8_t actuator_open(bms_dir_t dir);

/**
 * @brief       启动预充：只导通预充 MOS，⛔ 主开关保持断开
 * @retval      ACT_OK / ACT_ERR_STATE（主开关已闭合时拒绝）/ ACT_ERR_PIN
 * @note        预充 MOS 只承受预充电流，**不允许**承载负载电流 —— 所以它必须有人在
 *              窗口结束后关掉（`actuator_precharge_stop()`，正常路径；`actuator_open()`，异常路径）。
 */
uint8_t actuator_precharge_start(void);

/**
 * @brief       关闭预充 MOS
 * @retval      ACT_OK / ACT_ERR_PIN
 * @note        ★ 这一格是 §4.5 原契约里**缺**的一个函数（原表只有 `precharge_start`），
 *              补齐的口径与"为什么必须在闭合主开关**之后**才调它"见 `Tech_Design.md` §5.2
 *              （先合主、后断预充 = make-before-break：负载电流全程不断，
 *              而 10Ω 只在窗口里导电；反过来先断预充会让负载瞬间失去供电）。
 *              登记：`附录C` C-113。
 */
uint8_t actuator_precharge_stop(void);

/**
 * @brief       读回三个使能引脚的真实电平
 * @retval      `ACT_ST_CHG` / `ACT_ST_DSG` / `ACT_ST_PRE` 的位图
 * @note        开机自检与界面/协议用它做一次"意图 vs 现状"的对账（`app_protect.c`）：
 *              状态机认为自己在 `ON` 而这里读到全 0 = 有人把使能改掉了（或引脚被别的
 *              外设接管），两种都不可静默。
 */
uint8_t actuator_get_state(void);

#endif
