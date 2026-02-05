# BMS System (Battery Management System)

一个基于ESP32/ESP8266、针对ANT BMS开发的电池管理系统，支持自动电源切换、远程监控和OTA更新。

## 功能特性

### 🔋 BMS通信
- 通过串口与BMS(针对ANT BMS)通信（波特率19200）
- 实时读取电池数据：
  - 总电压
  - 电流
  - SOC（State of Charge）
  - MOS温度
  - 充电/放电MOS状态
- 通信故障检测和恢复

### ⚡ 智能电源管理
- 自动电源模式切换：
  - SOC ≤ 20% 时切换到市电模式
  - SOC ≥ 80% 时切换回电池模式（2分钟观察期）
- 通信失败时强制切换到市电（Fail-safe）
- GPIO2控制继电器实现电源切换

### 🌐 网络功能
- WiFi连接（支持ESP32和ESP8266）
- Web服务器提供状态监控页面
- Prometheus指标端点（`/metrics`）
- OTA固件更新（`/update`）

### 🔄 系统管理
- 定时重启：每24小时在市电供电下自动重启
- 硬件看门狗防止死机
- 重启计数和原因记录
- 非阻塞WiFi重连

## 硬件要求

- **微控制器**：ESP32 或 ESP8266
- **串口连接**：
  - ESP32：Serial1 (GPIO16 RX, GPIO17 TX)
  - ESP8266：Serial (复用调试串口)
- **电源控制**：GPIO2连接继电器线圈
- **电源**：支持电池和市电双路供电
- **接线** 上拉电阻 (针对 ESP8266)：建议在 GPIO2 和 3.3V 之间并联一个 4.7kΩ 或 10kΩ 的电阻。这能确保启动瞬间电平绝对稳定。


## 软件要求

- **开发环境**：PlatformIO
- **框架**：Arduino
- **依赖库**：
  - WiFi (ESP32/ESP8266)
  - WebServer
  - Update
  - EEPROM (ESP8266)

## 安装和配置

1. **克隆项目**：
   ```bash
   git clone <repository-url>
   cd BMSM
   ```

2. **配置WiFi**：
   编辑 `src/main.cpp` 中的WiFi凭证：
   ```cpp
   const char* ssid = "Your_WiFi_SSID";
   const char* password = "Your_WiFi_Password";
   ```

3. **编译和上传**：
   ```bash
   pio run -t upload
   ```

4. **监控**：
   ```bash
   pio device monitor
   ```

## 使用说明

### Web界面
- **状态页面**：`http://<device-ip>/`
  - 显示实时电池数据和电源模式
  - 自动刷新每5秒

- **Prometheus指标**：`http://<device-ip>/metrics`
  - 提供监控指标数据

- **OTA更新**：`http://<device-ip>/update`
  - 上传新固件进行在线更新

### 电源模式
- **电池模式**：使用电池供电
- **市电模式**：使用外部电源供电
- 系统自动根据SOC切换模式

### 定时重启
- 每24小时检查一次
- 仅在市电供电且不在电源切换观察期时重启
- 重启后重置计时器

## 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `BMS_BAUDRATE` | 19200 | BMS串口波特率 |
| `BMS_RESPONSE_LENGTH` | 140 | BMS响应数据长度 |
| `POWER_CONTROL_PIN` | 2 | 电源控制GPIO引脚 |
| `socThresholdLow` | 20 | 切换到市电的SOC阈值 |
| `socThresholdHigh` | 80 | 切换回电池的SOC阈值 |
| `SWITCH_CONFIRM_PERIOD` | 120000 | 电源切换观察期（毫秒） |
| `RESTART_INTERVAL` | 86400000 | 定时重启间隔（毫秒） |
| `BMS_COMM_FAIL_MAX` | 5 | 最大通信失败次数 |

## API端点

### GET /
返回系统状态的HTML页面。

### GET /metrics
返回Prometheus格式的监控指标：
- `bms_total_voltage_volts`：电池总电压
- `bms_current_amperes`：电池电流
- `bms_soc_percent`：电池SOC百分比
- `bms_mos_temperature_celsius`：MOS温度
- `power_mode_current`：当前电源模式
- `system_restart_count`：系统重启次数

### GET /update
返回OTA更新页面。

### POST /update
上传固件文件进行OTA更新。

## 故障排除

### BMS通信失败
- 检查串口连接和波特率设置
- 确认BMS设备正常工作
- 查看串口调试输出

### WiFi连接问题
- 确认WiFi凭证正确
- 检查网络信号强度
- 系统会自动重连

### 电源切换异常
- 检查继电器连接
- 确认GPIO2正常工作
- 查看SOC阈值设置

## 许可证

本项目采用MIT许可证。

## 版本历史
- v1.3：增加 EEPROM 数据有效性校验（Magic + Version）
- v1.2：该用低电平触发
- v1.1：添加定时重启功能，优化电源管理逻辑
- v1.0：初始版本，支持基本BMS通信和电源管理