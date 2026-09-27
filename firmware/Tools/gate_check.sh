#!/bin/sh
# ============================================================================
#  G7 机械可检门禁（AGENTS.md §3）—— 六条搜索，查到即改，不讨论
#  用法：  sh Tools/gate_check.sh        （在 firmware/ 下或任意目录均可）
#  退出码：0 = 全绿；非 0 = 有违规项（打印文件:行号）
# ============================================================================
cd "$(dirname "$0")/.." || exit 2
fail=0

echo "== G7-1  User/App 内不得出现 AFE 寄存器名 / 执行器引脚宏（S15）=="
if grep -rnE 'REG_[A-Z0-9_]+|CTRL[12]_[A-Z]+|SYS_STAT|CELLBAL|OV_TRIP|UV_TRIP|ADCGAIN|ADCOFFSET|CC_READY|CHG_ON|DSG_ON|AFE_BOOT_GPIO|AFE_HAL_|EN_CHG|EN_DSG|EN_PRE|afe_hal\.h' \
      --include='*.c' --include='*.h' User/App; then
    echo "   ^^ FAIL: 应用层看见了寄存器层的东西"; fail=1
else
    echo "   OK"
fi

echo "== G7-2  本工程代码不得出现 malloc / pvPortMalloc 调用（全静态分配）=="
# 只看自己写的三层；Middlewares/ 里 FreeRTOS 自己的定义与内部实现不算调用方
# 搜索根 = User/ Drivers/（本工程不用 CubeMX 默认的 Core/，生成代码在 User/ 下）
if grep -rnE '(^|[^A-Za-z0-9_.])(malloc|pvPortMalloc)[[:space:]]*\(' \
      --include='*.c' --include='*.h' User Drivers; then
    echo "   ^^ FAIL: 出现动态内存分配"; fail=1
else
    echo "   OK"
fi

echo "== G7-3  应用层（任务体所在文件）不得出现 HAL_Delay / delay_ms（S12）=="
# ★ D5（§8.4 M8）：原来这里排除 `app_main.c`（"遗留超循环"）。调度器已接管、该文件的
#   `while(1)` 已切成 `app_main_service()`，**排除项按计划删除** —— 留着就等于允许新代码往里写阻塞。
# S12 的口径是"**>10ms** 必须用 vTaskDelay"，所以 delay_ms 只查参数 ≥11 的调用；
# HAL_Delay 一律禁止（它在任务里等价于忙等，且没有让出调度器的可能）。
# ⚠️ 本条会搜到**注释里的** `delay_ms(20)`，这是有意的：注释里的示例代码会被下一个人照抄，
#    而且"门禁搜到注释也改内容、不放宽脚本"是本项目定下的口径（`附录C` 验收纪律）。
if grep -rnE 'HAL_Delay[[:space:]]*\(|delay_ms[[:space:]]*\((1[1-9]|[2-9][0-9]|[0-9]{3,})[[:space:]]*\)' \
      --include='*.c' User/App; then
    echo "   ^^ FAIL: 任务里长阻塞，改用 vTaskDelay"; fail=1
else
    echo "   OK"
fi

echo "== G7-4  看门狗不得存在可关闭的编译期开关（S13 / C-36）=="
# 交付版里出现 `#if APP_ENABLE_IWDG` 这种开关 = 允许"顺手把狗关掉"，
# 而狗一关，"软件卡死"就变成**永久静默**（S2 失效安全的最后兜底全在它身上）。
# 调试期要停狗只允许用 DBGMCU 的 IWDG 冻结位，不许用条件编译。
# ⚠️ 口径：只查**预处理指令**（`#if/#ifdef` 条件里出现 IWDG，或 `#define APP_ENABLE_IWDG`），
#    不查普通文本 —— 否则"解释为什么删掉这个开关"的注释本身会误报。
if grep -rnE '#[[:space:]]*if[n]?[[:space:]]+[^[:space:]].*IWDG|#define[[:space:]]+APP_ENABLE_IWDG' \
      --include='*.c' --include='*.h' User Drivers; then
    echo "   ^^ FAIL: 出现了能关掉看门狗的开关（S13）"; fail=1
else
    echo "   OK"
fi

echo "== G7-5  看门狗寄存器只允许出现在 app_protect.c（D5 §8.4 M4：喂狗点收口）=="
# "条件喂狗只允许出现在 TaskProtect"这条纪律，原来只能靠人读代码保证（数 IWDG_FEED 的文本
# 次数）—— 那是个**看不出真违规**的口径：任何人再写一句 `IWDG->KR = 0xAAAA` 就悄悄变成
# 无条件喂狗，判活全废而机器仍正常跑。这里改搜**寄存器实体**：一个文件之外出现即红。
if grep -rn 'IWDG->KR' --include='*.c' --include='*.h' User Drivers \
      | grep -v '^User/App/app_protect\.[ch]:'; then
    echo "   ^^ FAIL: 出现了第二个碰狗的文件（含注释里写到该寄存器的——改成文字描述）"; fail=1
else
    echo "   OK"
fi

echo "== G7-6  告警帧写者必须唯一：app_data_link_check() 的调用点恰好 1 处（C-97 F1）=="
# 判据为什么是"调用点计数"而不是"看谁 commit"：D5 之前这个调用长在**假数据源**尾部，
# 悄悄留第二处（例如又在 app_acq 里补一次）不会有任何编译错误，只会得到偶发撕裂帧。
# ⚠️ 口径：只数**真实调用语句**。本工程的注释纪律是用反引号包裹标识符（写作 `foo()`），
#    所以"名字前有反引号"= 文档提及，不算调用；声明/定义行（void/extern 开头）也不算。
#    这里与 G7-3、G7-5 故意不同：那两条搜的是"代码实体的字面量"，注释里写出来一样判红；
#    本条搜的是"计数"，把注释也计进去会得到一个没有信息量的数字（人人都改成"链路检查函数"）。
callers=$(grep -rnE 'app_data_link_check[[:space:]]*\(' --include='*.c' --include='*.h' User \
          | grep -vE '(void|extern)[[:space:]]+app_data_link_check' \
          | grep -vE '`[[:space:]]*app_data_link_check')
n=$(printf '%s' "$callers" | grep -c .)
if [ "$n" -ne 1 ]; then
    echo "$callers"
    echo "   ^^ FAIL: 命中 $n 处调用（应为 1 处，且必须在 app_protect.c 的 TaskProtect 路径上）"; fail=1
else
    echo "   OK（$callers）"
fi

[ $fail -eq 0 ] && echo "---- G7 全绿 ----" || echo "---- G7 有违规，见上 ----"
exit $fail
