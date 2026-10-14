# 🔋 BMS 磷酸铁锂监测终端

![License](https://img.shields.io/badge/license-MIT-green) ![Language](https://img.shields.io/badge/language-C-555555) ![Platform](https://img.shields.io/badge/platform-STM32F103ZET6-blue) ![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-orange) ![Build](https://img.shields.io/badge/build-0%20Error-brightgreen)

> 基于 STM32 + FreeRTOS 的 4S 磷酸铁锂电池组监测终端，五重保护 + SOC 估算 ⚡

## 这是什么？

一个**工业级**的电池管理系统（BMS），实时监测 4S 磷酸铁锂电池组的电压/电流/温度，进行 SOC 估算和五重保护，支持 Modbus RTU 和 MQTT 双通信。

**适合场景**：
- 🔋 电池管理系统（BMS）学习
- 📚 FreeRTOS 实战项目
- 🛡️ 安全关键系统设计（保护机制）

## ✨ 功能特性

### 数据采集与估算
- 📊 **SOC 估算**：安时积分 + 开路电压校正（双模互补）
- 📈 **多通道采集**：电压/电流/温度实时监测

### 五重保护
- ⚡ 过压 / 欠压
- 🔥 过流 / 过温 / 低温
- 🐕 看门狗独立喂狗

### 通信接口
- 📡 **Modbus RTU**：RS485 从机
- ☁️ **MQTT**：ESP8266 上报 OneNET

### 工程特性
- 🔒 **全静态内存**：无堆碎片风险
- ✅ **机械门禁**：9 条 grep 规则静态检查
- 🖥 **LCD 显示**：实时状态 + 告警界面

## 📁 目录结构

```
bms-monitor/
├── firmware/
│   ├── User/App/          # 应用层（17 个模块）
│   │   ├── app_acq.c      # 数据采集           
│   │   ├── app_soc.c      # SOC 估算           
│   │   ├── app_protect.c  # 保护判据           
│   │   ├── app_modbus.c   # Modbus 从机        
│   │   └── app_mqtt.c     # MQTT 客户端        
│   ├── Drivers/           # HAL + BSP          
│   └── Middlewares/       # FreeRTOS           
├── tools/                 # 门禁脚本           
├── PRD.md                 # 产品需求           
└── Tech_Design.md         # 技术设计           
```

## 🚀 快速开始

```bash
# 构建固件
cd firmware/Projects/MDK-ARM
UV4.exe -r bms.uvprojx -j0 -o build.log

# 运行静态门禁检查
bash tools/gate_check.sh
```

**构建状态**：0 Error / 0 Warning

## 📚 文档

| 文档 | 内容 |
|------|------|
| [PRD.md](PRD.md) | 产品需求文档 |
| [Tech_Design.md](Tech_Design.md) | 技术设计与架构 |
| [验收执行清单.md](验收执行清单.md) | 验收流程 |

## 📄 许可证

[MIT License](LICENSE) © y0u41

- 🔋 **全静态内存分配**：`configSUPPORT_DYNAMIC_ALLOCATION=0` 链接级强制，无堆碎片风险
- 📊 **SOC 估算**：安时积分 + 开路电压校正，双模互补
- 🛡️ **五重保护**：过压 / 欠压 / 过流 / 过温 / 低温，看门狗独立喂狗收口
- 📡 **双通信**：Modbus RTU 从机（RS485）+ OneNET MQTT 上报（ESP8266）
- 🖥️ **本地显示**：LCD 实时状态 / 告警界面
- ✅ **机械门禁**：9 条 grep 规则静态检查（`gate_check.sh`），0 违规才放行

## 系统架构

```text
AFE 采样板 → ADC 多通道采集 → app_acq → SOC 估算(app_soc) → 保护判据(app_protect)
                                    ↓                              ↓
                              app_data → app_ui(LCD)         app_alarm → 蜂鸣器/告警帧
                                    ↓
                        ┌───────────┼───────────┐
                        ↓           ↓           ↓
                   app_modbus   app_mqtt    app_log
                   (RS485从机)  (OneNET)   (FLASH历史)
```

## 项目结构

```
bms-monitor/
├── firmware/
│   ├── User/App/          # 应用层（17 个模块）
│   │   ├── app_acq.c      # 数据采集调度       
│   │   ├── app_soc.c      # SOC 估算           
│   │   ├── app_protect.c  # 保护判据           
│   │   ├── app_alarm.c    # 告警管理           
│   │   ├── app_modbus.c   # Modbus RTU 从机    
│   │   ├── app_mqtt.c     # MQTT 客户端        
│   │   ├── app_ui.c       # LCD 显示           
│   │   └── ...                                 
│   ├── Drivers/           # HAL + BSP          
│   ├── Middlewares/       # FreeRTOS           
│   └── Projects/MDK-ARM/  # Keil 工程          
├── tools/                 # 门禁脚本 + 取证工具
├── PRD.md                 # 产品需求           
├── Tech_Design.md         # 技术设计           
└── 验收执行清单.md         # 验收流程          
```

## 快速开始

```bash
# 构建固件（Keil MDK-ARM）
cd firmware/Projects/MDK-ARM
UV4.exe -r bms.uvprojx -j0 -o build.log

# 运行静态门禁检查
bash tools/gate_check.sh
```

## 资源占用（实测）

| 项 | 值 | 占比 |
|---|---|---|
| Flash | 87036 B (85.0 KB) | 512 KB 的 **16.6%** |
| RAM | 24104 B (23.5 KB) | 64 KB 的 **36.8%** |

## 硬件平台

| 外设 | 型号/接口 | 说明 |
|------|-----------|------|
| MCU | STM32F103ZET6 | 主控 |
| AFE | 电池模拟前端采样板 | 电压/电流/温度采集 |
| 显示 | LCD | 状态/告警界面 |
| 通信 | RS485 + ESP8266 | Modbus RTU + MQTT |
| RTOS | FreeRTOS V10.4.6 | 全静态分配 |

## 文档

| 文档 | 说明 |
|------|------|
| [PRD.md](PRD.md) | 产品需求文档 |
| [Tech_Design.md](Tech_Design.md) | 技术设计方案 |
| [验收执行清单.md](验收执行清单.md) | 验收流程与记录 |
| [Build.md](Build.md) | 构建与版本记录 |

## 许可证

[MIT License](LICENSE) © y0u41
