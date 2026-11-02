# AI-Safety-Inspection

基于 ESP32-S3 的 AI 安全巡检终端。当前处于硬件驱动验证阶段：双路 INMP441 数字麦克风采集 + I2S 回放（MAX98357），BSP 中已包含 WiFi 与 LED 驱动。

## 硬件

| 模块 | 说明 |
| --- | --- |
| MCU | ESP32-S3 |
| 麦克风 | INMP441 ×2（I2S 输入，双麦） |
| 功放 | MAX98357（I2S 输出） |
| Flash | 16 MiB（分区表见 `partitions-16MiB.csv`） |

## 目录结构

```
components/BSP/       板级驱动
  I2S/                双麦采集与 I2S 回放
  WIFI/               WiFi STA 连接
  LED_Driver/         LED 驱动
main/                 应用入口
partitions-16MiB.csv  自定义分区表
```

## 环境要求

- ESP-IDF v5.4
- 目标芯片：`esp32s3`

## 编译与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## WiFi 凭据配置（首次编译必做）

仓库中**不包含**任何 WiFi 账号或密码。首次编译前请复制模板并填写：

```bash
cp components/BSP/WIFI/wifi_credentials.h.example components/BSP/WIFI/wifi_credentials.h
# 然后编辑 wifi_credentials.h，填入 WIFI_ID 与 WIFI_PASSWORD
```

`wifi_credentials.h` 已加入 `.gitignore`，不会被提交。

## 组件依赖

依赖声明在 `main/idf_component.yml`，`idf.py reconfigure` 时会自动下载到 `managed_components/`（该目录不入库），版本由 `dependencies.lock` 锁定：

- `espressif/openai` ^1.1.0
- `espressif/mdns` ^1.11.1
