# Zephyr Health Monitoring System

基于 Zephyr RTOS 和 ESP32-S3 的开源健康监测设备项目，简称 ZHMS（Zephyr Health Monitoring System）。项目面向学习、原型开发和嵌入式健康数据采集验证，当前适配 ESP32-S3 Touch LCD 1.69 英寸开发板。

> 本项目仍在开发中，测量结果仅供工程验证和学习参考，不是医疗器械，也不能用于医疗诊断。

## 功能概览

- ST7789V2 240 × 280 LCD 显示与 CST816D 电容触摸。
- LVGL 三页面 UI：时钟、Wi-Fi 信息和心率页面，支持左右滑动切换。
- ESP32-S3 Wi-Fi STA、DHCP 和 SNTP 时间同步。
- 外接 MAX30102 PPG 采集，以及当前的 BPM、HRV、压力指数处理链路。
- 可选 MQTT BPM 上报；默认关闭，不依赖任何开发者的局域网 broker。
- 本地 Zephyr MAX30102 驱动、设备树绑定和板级 overlay。

## 当前支持状态

| 模块 | 当前状态 |
| --- | --- |
| ESP32-S3、LCD、触摸 | 已包含板级配置和应用代码 |
| Wi-Fi、DHCP、SNTP | 已包含，凭据需要本地配置 |
| MAX30102 | 已包含本地驱动、采集线程和算法线程 |
| MQTT | 已包含 BPM 上报，默认关闭，需要用户提供 broker |
| QMI8658C、PCF85063A、蜂鸣器、电池 ADC | 已记录硬件资源，当前应用尚未完整接入 |

## 硬件要求

### 主板

- ESP32-S3 Touch LCD 1.69 开发板。
- Zephyr 构建目标：`esp32s3_devkitc/esp32s3/procpu`。
- USB-C 数据线，用于供电、烧录和 USB 串口日志。

### MAX30102 接线

MAX30102 与触摸、RTC、IMU 共用 I2C 总线：

| MAX30102 信号 | ESP32-S3 |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| SCL | GPIO10 |
| SDA | GPIO11 |
| INT | GPIO18 |
| I2C 地址 | `0x57` |

LCD、触摸屏和完整引脚表请参阅[开发板硬件资源说明](docs/硬件资源说明/开发板硬件资源说明.md)。

## 软件环境

本文以 Linux/macOS 的 Bash 环境为例。需要准备：

- Git、CMake、Ninja 和 Python。
- `west` 及 Zephyr Python 依赖。
- 与目标 Zephyr 版本匹配的 Zephyr SDK。
- 当前工程开发环境使用 Zephyr `v4.4.0-rc1`，建议先使用同一版本。
- 建议使用 Python 3.12。

本仓库是 Zephyr 应用工程，不把 Zephyr 源码、模块、SDK 或 Python 虚拟环境提交到仓库。你可以使用已有 Zephyr 工作区，也可以按照 Zephyr 官方的[环境搭建文档](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)创建工作区。

### 使用已有 Zephyr 工作区

下面的路径只是示例，请替换成你自己的 Zephyr 工作区路径：

```bash
source "$HOME/zephyrproject/.venv/bin/activate"
export ZEPHYR_BASE="$HOME/zephyrproject/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
```

确认环境：

```bash
which west
west --version
west topdir
echo "$ZEPHYR_BASE"
```

然后下载本项目：

```bash
git clone https://github.com/SongChengMing1/ZHMS.git zhms
cd zhms
```

如果你还没有 Zephyr 工作区，请先按官方文档完成 `west init`、`west update` 和 SDK 安装，再执行上面的项目下载步骤。

## 本地配置

Wi-Fi 密码、MQTT broker 地址和 topic 都不应提交到 Git。复制配置模板：

```bash
cp config/zhms.local.conf.example config/zhms.local.conf
```

编辑 `config/zhms.local.conf`，至少填写 Wi-Fi 配置：

```conf
CONFIG_ZHMS_WIFI_SSID="your-wifi-ssid"
CONFIG_ZHMS_WIFI_PSK="your-wifi-passphrase"
```

如果需要 MQTT BPM 上报，再启用并填写：

```conf
CONFIG_ZHMS_MQTT_ENABLED=y
CONFIG_ZHMS_MQTT_BROKER_ADDR="192.168.1.100"
CONFIG_ZHMS_MQTT_BROKER_PORT=1883
CONFIG_ZHMS_MQTT_CLIENT_ID="zhms-devkitc-01"
CONFIG_ZHMS_MQTT_TOPIC="zhms/devkitc-01/health/bpm"
```

没有 MQTT broker 时，将 `CONFIG_ZHMS_MQTT_ENABLED` 设置为 `n`。


## 编译、烧录和串口监控

在项目根目录执行首次构建：

```bash
west build -p always \
  -b esp32s3_devkitc/esp32s3/procpu \
  -- -DEXTRA_CONF_FILE=config/zhms.local.conf
```

项目的 `CMakeLists.txt` 会自动使用 `boards/esp32s3_devkitc.overlay`。修改 C 代码后的增量构建：

```bash
west build -p auto \
  -b esp32s3_devkitc/esp32s3/procpu \
  -- -DEXTRA_CONF_FILE=config/zhms.local.conf
```

连接开发板后烧录：

```bash
west flash
```

查看 ESP32-S3 串口日志：

```bash
west espressif monitor
```

也可以使用通用串口工具：

```bash
python -m serial.tools.miniterm <PORT> 115200
```

启动后，串口会输出类似 `ZHMS init ok` 的启动日志；MAX30102 服务启用后会输出 `bpm,hrv,stress_level` 表头及后续摘要。实际输出会受到 Wi-Fi、传感器接触和算法状态影响。

## 项目结构

```text
.
├── boards/                 # ESP32-S3 板级 Kconfig 和 devicetree overlay
├── config/                 # 本地部署配置模板
├── drivers/                # 本地 MAX30102 Zephyr sensor 驱动
├── dts/bindings/           # MAX30102 devicetree binding
├── src/
│   ├── services/           # Wi-Fi、时间同步、MAX30102、健康指标、MQTT
│   └── ui/                 # LCD、LVGL 页面和触摸交互
├── docs/                   # 硬件、开发指南、算法和问题记录
├── CMakeLists.txt
├── Kconfig
├── prj.conf
└── README.md
```

## 文档

- [开发板硬件资源说明](docs/硬件资源说明/开发板硬件资源说明.md)

## 已知限制

- 心率、HRV 和压力指数仅用于工程验证，不代表医学测量结论。
- 本项目仍在持续迭代，硬件支持范围和配置项可能随版本变化。

## 许可证

本项目采用 [MIT License](LICENSE)。
