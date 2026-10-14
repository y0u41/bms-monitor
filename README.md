# BMS 磷酸铁锂监测终端

![License](https://img.shields.io/badge/license-MIT-green) ![Language](https://img.shields.io/badge/language-C-555555) ![Platform](https://img.shields.io/badge/platform-STM32F103ZET6-blue) ![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-orange)

基于 STM32F103ZET6 + FreeRTOS 的 4S 磷酸铁锂电池组监测终端，实现电压/电流/温度采集、SOC 估算、
充放电保护、告警上报与本地显示。

## 特性

- **全静态内存分配**：`configSUPPORT_DYNAMIC_ALLOCATION=0`，链接级强制，无堆碎片风险
- **SOC 估算**：安时积分 + 开路电压校正
- **保护判据**：过压/欠压/过流/过温/低温五重保护，看门狗独立喂狗收口
- **通信接口**：Modbus RTU 从机 + OneNET MQTT 上报
- **本地显示**：LCD 状态/告警界面
- **机械门禁**：9 条 grep 规则静态检查（`gate_check.sh`）

## 硬件

| 外设 | 说明 |
|------|------|
| MCU | STM32F103ZET6 |
| AFE | 电池模拟前端采样板 |
| 显示 | LCD |
| 通信 | RS485 (Modbus) + ESP8266 (MQTT) |

## 构建

```bash
# Keil MDK-ARM
UV4.exe -r bms.uvprojx -j0 -o build.log
```

## 文档

- `PRD.md` - 产品需求
- `Tech_Design.md` - 技术设计
- `验收执行清单.md` - 验收流程
- `Build.md` - 构建记录

## 许可证

MIT License
