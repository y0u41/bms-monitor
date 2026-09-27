# EasyEDA（嘉立创EDA 专业版）AI 网关 · 本机速查

来源：官方 skill `easyeda-api` v1.1.28（zip：`https://image.lceda.cn/files/easyeda-api-skill.zip`），解压到本目录，依赖只有 `ws`（已 `npm install`）。
本文件只是**工具速查**，不产生任何项目规格；画板口径一律看 `../Tech_Design.md` §3.1.1 与 `../画板指南_AFE采样板.md`。

## 链路

```
AI/终端  ──HTTP──►  Bridge Server :49620-49629  ──WebSocket──►  嘉立创EDA 扩展 Run API Gateway
```

EDA 侧要求：专业版已安装 `run-api-gateway.eext`，且在扩展管理器里勾选 **允许外部交互** + **显示在顶部菜单**。

## 三步自检

```bash
# 1) 找网关并确认 EDA 已连（关键字段：edaConnected / edaWindowCount）
for p in $(seq 49620 49629); do curl -s --max-time 2 http://127.0.0.1:$p/health | grep -q easyeda-bridge && echo "PORT=$p" && curl -s http://127.0.0.1:$p/health && break; done
# 2) 确认窗口（1 个窗口会自动成为 active；2 个以上要 POST /eda-windows/select）
curl -s http://127.0.0.1:49620/eda-windows
# 3) 真跑一次 API（唯一可信的连通判据 —— health 返回 ok 不代表 EDA 端能执行）
curl -s -X POST http://127.0.0.1:49620/execute -H "Content-Type: application/json" \
  -d '{"code":"return await eda.dmt_Project.getCurrentProjectInfo();"}'
```

第 1 步若无端口：`node scripts/bridge-server.mjs`（脚本自带单例检测，已有实例时会直接退出）。

## 调用约定（写在 payload 里，别塞进 shell 参数）

代码在 EDA 的浏览器运行时里以 `async function(eda){ ...你的代码... }` 执行：

- **必须 `return`**，`console.log` 不会被回收；
- **必须 `await`**（几乎所有 API 返回 `Promise`）；
- **不得写注释**（按单行执行）；
- **枚举用名字不用数字**（如 `EPCB_LayerId.TOP`，见 `references/enums/`）；
- 无 Node API（`fs`/`path` 都没有）；
- 单位：**PCB = 1mil**，**原理图 = 0.01inch(10mil)** —— 搞混就偏 10 倍。

含中文的代码走文件更稳：`curl -d @payload.json`（PowerShell/CJK 参数转义极易出错）。

## 操作纪律（针对本工程）

| 动作 | 判据 |
|---|---|
| `dmt_Project.createProject()` 之后 | ⛔ **不要顺手 `openProject()`** —— 它会**丢弃当前工程未保存的改动**（官方文档明写）。要先保存/关闭再切 |
| 任何 `pcb_*` / `sch_*` 调用之前 | 先 `dmt_Project.getCurrentProjectInfo()` + `dmt_SelectControl.getCurrentDocumentInfo()` 确认**打开的文档类型对**（PCB 文档才准调 `pcb_*`） |
| 改图元 | 用 async 图元模式：`get()` → `toAsync()` → `setState_*()` → `done()` |
| 反复"其它 API 正常、这一个恒返回 null/报错" | 按 **权限/版本受限**处理，不要当代码 bug 硬试 |

API 查表顺序：`references/_index.md` → `references/classes/<类>.md`（读完整签名与"备注"）→ `references/_quick-reference.md` 快速比对。
