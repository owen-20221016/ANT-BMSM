#include <Arduino.h>

// 新增：支持 ESP32 / ESP8266 的 WiFi + WebServer + OTA 更新
#if defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
WebServer server(80);
#include "esp_task_wdt.h"
#include "esp_system.h"
#else
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
#include <EEPROM.h>
ESP8266WebServer server(80);
#endif
#if defined(ESP32)
  #define BMS_SERIAL Serial1
#else
  #define BMS_SERIAL Serial
#endif
// OTA 配置（请替换为你的 WiFi 凭证）
const char* ssid = "Your_WiFi_SSID";
const char* password = "Your_WiFi_Password";

// ==================== BMS 通信配置 ====================
// 波特率: 19200, 数据位: 8, 奇偶校验: 无, 停止位: 1
const uint32_t BMS_BAUDRATE = 19200;
const uint8_t BMS_RESPONSE_LENGTH = 140; // BMS 回复 140 字节
bool pendingRestart = false;
// BMS 数据结构体
struct BMSData {
  // 帧头
  uint8_t frameHeader[4]; // 0xAA, 0x55, 0xAA, 0xFF
  
  // 总压: Data4(高) Data5(低), 0.1V/bit
  uint16_t totalVoltage;
  float totalVoltageV;
  
  // 单体电压: Data6-Data69 (32个单体), 1mV/bit
  // uint16_t cellVoltages[32];
  // float cellVoltagesV[32];
  
  // 电流: Data72(高) Data73(低), 0.1A/bit
  uint16_t current;
  float currentA;
  
  // SOC: Data74, 1%/bit
  uint8_t soc;
  
  // 物理容量: Data75-Data78 (4字节), 0.000001AH/bit
  // uint32_t physicalCapacity;
  // float physicalCapacityAh;
  
  // 剩余容量: Data79-Data82 (4字节), 0.000001AH/bit
  // uint32_t remainingCapacity;
  // float remainingCapacityAh;
  
  // 循环容量: Data83-Data86 (4字节), 0.000001AH/bit
  // uint32_t cycleCapacity;
  // float cycleCapacityAh;
  
  // 系统时间: Data87-Data90 (4字节), 1s/bit
  // uint32_t systemTime;
  
  // MOS 温度: Data91(高) Data92(低), 1℃/bit
  int16_t mosTemp;
  
  // 均衡温度: Data93(高) Data94(低), 1℃/bit
  // int16_t balanceTemp;
  
  // 外部温度: Data95-Data102 (4个温度), 1℃/bit
  // int16_t externalTemp[4];
  
  // MOS 管状态: Data103
  uint8_t chargeMOSStatus;
  
  // 放电 MOS 状态: Data104
  uint8_t dischargeMOSStatus;
  
  // 均衡状态: Data105
  // uint8_t balanceStatus;
  
  // 最高单体电压信息
  // uint8_t maxCellIndex;
  // uint16_t maxCellVoltage;
  // float maxCellVoltageV;
  
  // 最低单体电压信息
  // uint8_t minCellIndex;
  // uint16_t minCellVoltage;
  // float minCellVoltageV;
  
  // 平均单体电压
  // uint16_t avgCellVoltage;
  // float avgCellVoltageV;
  
  // 实际串数
  // uint8_t actualCellCount;
  
  // 系统日志: Data136(高) Data137(低)
  // uint16_t systemLog;
  
  // 校验和: Data138(高) Data139(低)
  // uint16_t checksum;
};

BMSData bmsData;
// ==================== 电源管理配置 ====================
// GPIO2 用于控制继电器线圈：
//  - HIGH (继电器不吸合) -> 继电器回到 NC -> 默认使用市电 (Fail-safe)
//  - LOW (继电器吸合)   -> 切换到 NO -> 使用电池
const uint8_t POWER_CONTROL_PIN = 2;

// 电源模式枚举
enum PowerMode {
  BATTERY_MODE = 0,      // 使用电池
  AC_POWER_MODE = 1      // 使用市电
};

// 电源管理状态
struct PowerManager {
  PowerMode currentMode;           // 当前电源模式
  PowerMode lastMode;              // 上次电源模式
  uint8_t socThresholdLow;         // SOC 低阈值 (切换到市电)
  uint8_t socThresholdHigh;        // SOC 高阈值 (切换回电池)
  unsigned long lastModeChangeTime; // 上次模式切换时间
  uint16_t modeChangeCount;        // 模式切换次数（计数）
};

PowerManager powerMgr = {
  .currentMode = AC_POWER_MODE,
  .lastMode = AC_POWER_MODE,
  .socThresholdLow = 20,           // 电量 <= 20% 切换到市电
  .socThresholdHigh = 80,          // 电量 >= 80% 切换回电池
  .lastModeChangeTime = 0,
  .modeChangeCount = 0
};

// BMS 连续通信失败计数，超过阈值切换到市电（Fail-safe）
uint8_t bmsCommFailCount = 0;
const uint8_t BMS_COMM_FAIL_MAX = 5;

// 当因通信失败强制切换到市电时设置标志并记录时间，恢复前观察期
bool forcedACByCommFail = false;
unsigned long forcedACStartTime = 0;

// 核心计时器：用于所有切回电池动作的 2 分钟确认
unsigned long batteryModeTargetTime = 0; 
const unsigned long SWITCH_CONFIRM_PERIOD = 120000; // 120秒


// ==================== 定时重启配置 ====================
unsigned long lastRestartTime = 0;
const unsigned long RESTART_INTERVAL = 86400000UL; // 24小时


// 重启计数（持久化）：ESP32 使用 RTC_DATA_ATTR，ESP8266 使用 EEPROM
#if defined(ESP32)
RTC_DATA_ATTR uint32_t rtc_restart_count = 0;
#endif
uint32_t restartCount = 0;
// 会话内累计通信失败次数（仅本次启动有效）
uint32_t bmsCommTotalFailCount = 0;

// WiFi 重连机制（非阻塞）
const unsigned long WIFI_RECONNECT_INTERVAL = 30000; // 30s 重连间隔
bool wifiConnectedFlag = false; // 上次连接状态标志

// ==================== Prometheus 监控 ===================="

// 生成 Prometheus 格式的指标数据
String generatePrometheusMetrics() {
  // 预分配以减少动态内存分配与堆碎片（在内存受限的 MCU 上重要）
  String metrics = "";
  metrics.reserve(1600);
  
  // BMS 总电压指标
  metrics += "# HELP bms_total_voltage_volts Total battery voltage in volts\n";
  metrics += "# TYPE bms_total_voltage_volts gauge\n";
  metrics += "bms_total_voltage_volts " + String(bmsData.totalVoltageV, 1) + "\n\n";
  
  // 电流指标
  metrics += "# HELP bms_current_amperes Battery current in amperes\n";
  metrics += "# TYPE bms_current_amperes gauge\n";
  metrics += "bms_current_amperes " + String(bmsData.currentA, 1) + "\n\n";
  
  // SOC 指标
  metrics += "# HELP bms_soc_percent State of charge percentage\n";
  metrics += "# TYPE bms_soc_percent gauge\n";
  metrics += "bms_soc_percent " + String(bmsData.soc) + "\n\n";
  
  // MOS 温度指标
  metrics += "# HELP bms_mos_temperature_celsius MOS temperature in Celsius\n";
  metrics += "# TYPE bms_mos_temperature_celsius gauge\n";
  metrics += "bms_mos_temperature_celsius " + String(bmsData.mosTemp) + "\n\n";
  
  // 充电 MOS 状态指标
  metrics += "# HELP bms_charge_mos_status Charge MOS status\n";
  metrics += "# TYPE bms_charge_mos_status gauge\n";
  metrics += "bms_charge_mos_status " + String(bmsData.chargeMOSStatus) + "\n\n";
  
  // 放电 MOS 状态指标
  metrics += "# HELP bms_discharge_mos_status Discharge MOS status\n";
  metrics += "# TYPE bms_discharge_mos_status gauge\n";
  metrics += "bms_discharge_mos_status " + String(bmsData.dischargeMOSStatus) + "\n\n";
  
  // 电源模式指标
  metrics += "# HELP power_mode_current Current power mode (0=battery, 1=ac_power)\n";
  metrics += "# TYPE power_mode_current gauge\n";
  metrics += "power_mode_current " + String(powerMgr.currentMode) + "\n";

  // BMS 通信失败计数
  metrics += "# HELP bms_comm_fail_count Consecutive BMS comm failure count\n";
  metrics += "# TYPE bms_comm_fail_count gauge\n";
  metrics += "bms_comm_fail_count " + String(bmsCommFailCount) + "\n";

  // 系统重启计数
  metrics += "# HELP system_restart_count Number of watchdog restarts\n";
  metrics += "# TYPE system_restart_count gauge\n";
  metrics += "system_restart_count " + String(restartCount) + "\n";

  // 会话内累计通信失败次数
  metrics += "# HELP bms_comm_total_fail_count Cumulative BMS comm failures since boot\n";
  metrics += "# TYPE bms_comm_total_fail_count gauge\n";
  metrics += "bms_comm_total_fail_count " + String(bmsCommTotalFailCount) + "\n";

  // 是否因通信失败强制切到市电
  metrics += "# HELP forced_ac_by_comm_fail Flag if forced to AC due to comm failures (0/1)\n";
  metrics += "# TYPE forced_ac_by_comm_fail gauge\n";
  metrics += "forced_ac_by_comm_fail " + String(forcedACByCommFail ? 1 : 0) + "\n";
  
  return metrics;
}
// 格式化时间差的函数
String getTimeAgo(unsigned long lastMs) {
    if (lastMs == 0) return "No Change";
    
    unsigned long diff = (millis() - lastMs) / 1000; // 计算秒数差
    
    if (diff < 60) {
        return String(diff) + "s ago";
    } else if (diff < 3600) {
        return String(diff / 60) + "m " + String(diff % 60) + "s ago";
    } else {
        unsigned long hrs = diff / 3600;
        unsigned long mins = (diff % 3600) / 60;
        return String(hrs) + "h " + String(mins) + "m ago";
    }
}

// ==================== BMS 通信函数 ====================

void sendBMSRequest() {
  // 第一步：清空串口缓冲区，确保没有之前的残余数据干扰
  while (BMS_SERIAL.available() > 0) {
    BMS_SERIAL.read();
  }

  // 第二步：只发送 6 字节原始指令，不带任何 println 或 printf
  const uint8_t BMS_QUERY_CMD[] = {0x5A, 0x5A, 0x00, 0x00, 0x00, 0x00};
  BMS_SERIAL.write(BMS_QUERY_CMD, 6);
  
  // 必须确保这之后没有任何 Serial.print 语句执行
  BMS_SERIAL.flush(); 
}

// 校验和计算和验证: 将 Data[4] 到 Data[137] 累加，结果应等于 Data[138]*256+Data[139]
bool verifyBMSChecksum(const uint8_t* data, uint8_t length) {
  if (length != BMS_RESPONSE_LENGTH) return false;
  
  uint16_t sum = 0;
  for (uint8_t i = 4; i <= 137; i++) {
    sum += data[i];
  }
  
  uint16_t checksumInData = ((uint16_t)data[138] << 8) | data[139];
  
  if (sum != checksumInData) {
    return false;
  }
  
  return true;
}

// 解析 BMS 回复数据（140 字节）
bool parseBMSData(const uint8_t* data, uint8_t length) {
  if (length != BMS_RESPONSE_LENGTH) {
    // Serial.printf("Invalid BMS data length: %d (expected 140)\n", length);
    return false;
  }
  
  // 验证帧头
  if (data[0] != 0xAA || data[1] != 0x55 || data[2] != 0xAA || data[3] != 0xFF) {
    // Serial.println("Invalid frame header!");
    return false;
  }
  
  // 验证校验和
  if (!verifyBMSChecksum(data, length)) {
    // Serial.println("Invalid Checksum！");
    return false;
  }
  
  // 保存帧头
  bmsData.frameHeader[0] = data[0];
  bmsData.frameHeader[1] = data[1];
  bmsData.frameHeader[2] = data[2];
  bmsData.frameHeader[3] = data[3];
  
  // 解析总压 (Data4-Data5): 0.1V/bit
  bmsData.totalVoltage = ((uint16_t)data[4] << 8) | data[5];
  bmsData.totalVoltageV = bmsData.totalVoltage * 0.1f;
  
  // 解析单体电压 (Data6-Data69): 1mV/bit, 共32个单体
  // for (uint8_t i = 0; i < 32; i++) {
  //   uint8_t highByte = data[6 + i * 2];
  //   uint8_t lowByte = data[7 + i * 2];
  //   bmsData.cellVoltages[i] = ((uint16_t)highByte << 8) | lowByte;
  //   bmsData.cellVoltagesV[i] = bmsData.cellVoltages[i] * 0.001f;
  // }
  
  // 解析电流 (Data72-Data73): 0.1A/bit
  bmsData.current = ((uint16_t)data[72] << 8) | data[73];
  bmsData.currentA = bmsData.current * 0.1f;
  
  // 解析 SOC (Data74): 1%/bit
  bmsData.soc = data[74];
  
  // 解析物理容量 (Data75-Data78): 0.000001AH/bit
  // bmsData.physicalCapacity = ((uint32_t)data[75] << 24) | ((uint32_t)data[76] << 16) | 
  //                            ((uint32_t)data[77] << 8) | data[78];
  // bmsData.physicalCapacityAh = bmsData.physicalCapacity * 0.000001f;
  
  // // 解析剩余容量 (Data79-Data82): 0.000001AH/bit
  // bmsData.remainingCapacity = ((uint32_t)data[79] << 24) | ((uint32_t)data[80] << 16) | 
  //                             ((uint32_t)data[81] << 8) | data[82];
  // bmsData.remainingCapacityAh = bmsData.remainingCapacity * 0.000001f;
  
  // // 解析循环容量 (Data83-Data86): 0.000001AH/bit
  // bmsData.cycleCapacity = ((uint32_t)data[83] << 24) | ((uint32_t)data[84] << 16) | 
  //                         ((uint32_t)data[85] << 8) | data[86];
  // bmsData.cycleCapacityAh = bmsData.cycleCapacity * 0.000001f;
  
  // // 解析系统时间 (Data87-Data90): 1s/bit
  // bmsData.systemTime = ((uint32_t)data[87] << 24) | ((uint32_t)data[88] << 16) | 
  //                      ((uint32_t)data[89] << 8) | data[90];
  
  // 解析 MOS 温度 (Data91-Data92): 1℃/bit (有符号)
  bmsData.mosTemp = ((int16_t)data[91] << 8) | data[92];
  
  // // 解析均衡温度 (Data93-Data94): 1℃/bit (有符号)
  // bmsData.balanceTemp = ((int16_t)data[93] << 8) | data[94];
  
  // // 解析外部温度 (Data95-Data102): 1℃/bit (有符号)，共4个温度
  // for (uint8_t i = 0; i < 4; i++) {
  //   bmsData.externalTemp[i] = ((int16_t)data[95 + i * 2] << 8) | data[96 + i * 2];
  // }
  
  // 解析 MOS 充电状态 (Data103)
  bmsData.chargeMOSStatus = data[103];
  
  // 解析 MOS 放电状态 (Data104)
  bmsData.dischargeMOSStatus = data[104];
  
  // 解析均衡状态 (Data105)
  // bmsData.balanceStatus = data[105];
  
  // // 解析最高单体电压信息 (Data115-Data117)
  // bmsData.maxCellIndex = data[115];
  // bmsData.maxCellVoltage = ((uint16_t)data[116] << 8) | data[117];
  // bmsData.maxCellVoltageV = bmsData.maxCellVoltage * 0.001f;
  
  // // 解析最低单体电压信息 (Data118-Data120)
  // bmsData.minCellIndex = data[118];
  // bmsData.minCellVoltage = ((uint16_t)data[119] << 8) | data[120];
  // bmsData.minCellVoltageV = bmsData.minCellVoltage * 0.001f;
  
  // // 解析平均单体电压 (Data121-Data122)
  // bmsData.avgCellVoltage = ((uint16_t)data[121] << 8) | data[122];
  // bmsData.avgCellVoltageV = bmsData.avgCellVoltage * 0.001f;
  
  // // 解析实际串数 (Data123)
  // bmsData.actualCellCount = data[123];
  
  // // 解析系统日志 (Data136-Data137)
  // bmsData.systemLog = ((uint16_t)data[136] << 8) | data[137];
  
  // 保存校验和
  // bmsData.checksum = ((uint16_t)data[138] << 8) | data[139];
  
  return true;
}

// 读取 BMS 数据
bool readBMSData(uint8_t* buffer, uint8_t maxLength) {
  uint32_t timeout = millis() + 2000; // 总限时 2 秒
  
  // 1. 寻找帧头 (防止卡死)
  bool foundHeader = false;
  while (millis() < timeout) {
    if (BMS_SERIAL.available() > 0) {
      if (BMS_SERIAL.read() == 0xAA) {
        buffer[0] = 0xAA;
        foundHeader = true;
        break; // 抓到了包头，跳出第一个 while
      }
    }
    // 这里不需要 delay，交给 loop 频率控制即可
  }

  // 如果连包头都没等到，直接返回失败，不会往下走
  if (!foundHeader) return false;

  // 2. 读取余下的包体 (同样带超时保护)
  uint8_t index = 1; 
  while (index < maxLength && millis() < timeout) {
    if (BMS_SERIAL.available() > 0) {
      buffer[index++] = BMS_SERIAL.read();
    }
  }
  
  // 3. 最终长度检查
  return (index == maxLength);
}
// 初始化电源管理GPIO
void initPowerControl() {
  pinMode(POWER_CONTROL_PIN, OUTPUT);
  // 初始状态：继电器不吸合 -> 默认使用市电（Fail-safe）
  digitalWrite(POWER_CONTROL_PIN, HIGH);
  powerMgr.currentMode = AC_POWER_MODE;
  powerMgr.lastMode = AC_POWER_MODE;
}

// 设置电源模式
void setPowerMode(PowerMode mode) {
  if (mode == powerMgr.currentMode) {
    return; // 模式未改变，无需操作
  }

  powerMgr.lastMode = powerMgr.currentMode;
  powerMgr.currentMode = mode;
  powerMgr.lastModeChangeTime = millis();
  powerMgr.modeChangeCount++;

  // 注意：HIGH = 继电器不吸合 -> 市电 (默认安全)
  if (mode == BATTERY_MODE) {
    digitalWrite(POWER_CONTROL_PIN, LOW); // 吸合 -> 切换到电池
  } else {
    digitalWrite(POWER_CONTROL_PIN, HIGH); // 释放 -> 回到市电
  }
}

// 电源管理逻辑控制函数
// 规则：
// 1. 当前为电池模式，SOC <= 20% -> 切换到市电模式
// 2. 当前为市电模式，SOC >= 80% -> 切换回电池模式（需等待2分钟观察期）
// 3. 否则保持当前模式
void updatePowerMode() {
  bool bmsAlive = (bmsCommFailCount < BMS_COMM_FAIL_MAX);
  bool socHigh = (bmsData.soc >= powerMgr.socThresholdHigh);
  bool socLow = (bmsData.soc <= powerMgr.socThresholdLow);

  // --- 场景 A：立即切向市电 (故障或低电量) ---
  if (!bmsAlive || socLow) {
    if (!bmsAlive) forcedACByCommFail = true; // 故障立标

    if (powerMgr.currentMode != AC_POWER_MODE) {
      setPowerMode(AC_POWER_MODE);
    }
    batteryModeTargetTime = 0; // 只要在市电，就重置电池切换计时
    return;
  }

  // --- 场景 B：尝试切向电池 (统一 2 分钟冷静期) ---
  if (powerMgr.currentMode == AC_POWER_MODE) {
    // 此时已经隐含：bmsAlive 是真的 且 socLow 是假的
    // 我们只需检查是否达到恢复电量 (80%)
    if (socHigh) {
      if (batteryModeTargetTime == 0) {
        batteryModeTargetTime = millis();
      } else if (millis() - batteryModeTargetTime >= SWITCH_CONFIRM_PERIOD) {
        setPowerMode(BATTERY_MODE);
        forcedACByCommFail = false; // 只有安检通过，才卸载故障标识
        batteryModeTargetTime = 0;
      }
    } else {
      batteryModeTargetTime = 0; // 条件不满足（比如SOC掉回79），计时重置
    }
  }
}

const char statusForm[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <meta http-equiv="refresh" content="5"> 
    <title>BMS Status v1.1</title>
    <style>
      body { font-family: 'Segoe UI', Arial; margin: 20px; background: #fafafa; }
      .container { background: {{color}}; padding: 20px; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); max-width: 500px; margin-bottom: 20px;}
      .item { display: flex; justify-content: space-between; margin: 8px 0; padding: 12px; background: white; border-radius: 8px; align-items: center;}
      .label { font-weight: bold; color: #555; }
      .value { color: #007bff; font-family: monospace; font-size: 1.1em; }
      h1 { color: #333; }
      input[type="number"] { width: 60px; padding: 5px; border-radius: 4px; border: 1px solid #ccc; }
      input[type="submit"] { width: 100%; padding: 10px; background: #333; color: white; border: none; border-radius: 8px; cursor: pointer; font-weight: bold; margin-top: 10px;}
    </style>
  </head>
  <body>
    <h1>🔋 BMS System Status</h1>
    <div class="container">
      <div class="item"><span class="label">Voltage</span><span class="value">{{v}} V</span></div>
      <div class="item"><span class="label">Current</span><span class="value">{{a}} A</span></div>
      <div class="item"><span class="label">SOC</span><span class="value">{{soc}} %</span></div>
      <div class="item"><span class="label">Mode</span><span class="value"><b>{{mode}}</b></span></div>
      <div class="item"><span class="label">Comm Fail Count</span><span class="value">{{comm_fail}}</span></div>
      <div class="item"><span class="label">Comm Fail Total</span><span class="value">{{comm_total}}</span></div>
      <div class="item"><span class="label">Restart Count</span><span class="value">{{restart}}</span></div>
      <div class="item"><span class="label">Last Change</span><span class="value">{{time}}</span></div>
      <div class="item"><span class="label">Switch Count</span><span class="value">{{count}}</span></div>
    </div>
  </body>
</html>)rawliteral";

// 简单的 OTA 上传页面（含进度）
const char updateForm[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8">
    <title>OTA Update</title>
  </head>
  <body>
    <h3>Firmware Update</h3>
    <form method="POST" action="/update" enctype="multipart/form-data" id="uploadForm">
      <input type="file" name="firmware">
      <input type="submit" value="Upload">
    </form>
    <div id="status"></div>
    <script>
      const form = document.getElementById('uploadForm');
      const status = document.getElementById('status');
      form.addEventListener('submit', function(e){
        e.preventDefault();
        const file = form.elements['firmware'].files[0];
        if(!file){ status.innerText = 'Select a file'; return; }
        const xhr = new XMLHttpRequest();
        xhr.open('POST','/update',true);
        xhr.upload.onprogress = function(e){
          if(e.lengthComputable) status.innerText = 'Uploading: ' + Math.floor((e.loaded/e.total)*100) + '%';
        };
        xhr.onload = function(){
          status.innerText = 'Result: ' + xhr.responseText;
        };
        const fd = new FormData();
        fd.append('firmware', file);
        xhr.send(fd);
      });
    </script>
  </body>
</html>
)rawliteral";

void setup() {
  // 新增：初始化串口与 WiFi
  // Serial0 用于 WiFi 调试 (115200)
  Serial.begin(19200);
  delay(100);

  #if defined(ESP32)
    // ESP32 增加接收缓冲区到 256 字节
    Serial.setRxBufferSize(256); 
  #elif defined(ESP8266)
    // ESP8266 比较特殊，通常在 Serial.begin 之后修改
    // 注意：部分核心版本可能不支持此方法，但 140 字节在 8266 上通常能勉强挤进去
  #endif
  
  // Serial.println("\n\n========== BMS System Start ==========");
  
  // 如果是 ESP32，可以使用 Serial1 作为 BMS 通信 (TTL/485, 19200)
  // 如果是 ESP8266，只能复用 Serial0，但会影响调试输出
  // 这里演示如何配置（根据你的硬件调整）
  #if defined(ESP32)
    // ESP32: 使用 Serial1 (GPIO16/17) 作为 BMS 接口
    Serial1.begin(BMS_BAUDRATE, SERIAL_8N1, 16, 17); // RX=16, TX=17
    // Serial.println("ESP32: Serial1 initialized for BMS at 19200 baud");
  #else
    // ESP8266: 只能复用 Serial，改变波特率
    // Serial.begin(BMS_BAUDRATE); // 如需改为19200，取消注释此行
    // Serial.println("ESP8266: Using Serial for both WiFi and BMS");
    // Serial.println("WARNING: Consider using a hardware UART converter for BMS");
  #endif

  // 初始化电源管理（GPIO2控制市电供电）
  initPowerControl();

  // 启动 WiFi（非阻塞，立即返回）
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // 检查上次重启原因，若为看门狗相关则计数（持久化存储）
  #if defined(ESP32)
    // ESP32: 使用 RTC_DATA_ATTR 存储重启计数
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_WDT || rr == ESP_RST_TASK_WDT) {
      rtc_restart_count++;
    }
    restartCount = rtc_restart_count;
  #else
    // ESP8266: 使用 EEPROM 持久化 32-bit 重启计数
    EEPROM.begin(512);
    uint32_t stored = 0;
    EEPROM.get(0, stored);
    String info = ESP.getResetInfo();
    if (info.indexOf("wdt") != -1 || info.indexOf("WDT") != -1) {
      stored++;
      EEPROM.put(0, stored);
      EEPROM.commit();
    }
    restartCount = stored;
  #endif

  // 启用硬件看门狗（防止 MCU 死机导致继电器保持吸合）
  // ESP32 使用 esp_task_wdt，ESP8266 使用 ESP.wdtEnable
  #if defined(ESP32)
    esp_task_wdt_init(10, true); // 10s 超时，panic = true
    esp_task_wdt_add(NULL);
  #else
    // ESP8266: 启用 8s 看门狗
    ESP.wdtEnable(8000);
  #endif

  // 路由：根页面显示 BMS 状态
server.on("/", HTTP_GET, [](){
  String html;
  html.reserve(2048); // 预分配 HTML 缓冲以减少堆分配
  html = String(statusForm);
    
    html.replace("{{color}}", (powerMgr.currentMode == BATTERY_MODE) ? "#e3f2fd" : "#e8f5e9");
    html.replace("{{v}}", String(bmsData.totalVoltageV, 1));
    html.replace("{{a}}", String(bmsData.currentA, 1));
    html.replace("{{soc}}", String(bmsData.soc));
    
    // 组合模式字符串
    String modeStr;
    modeStr.reserve(64);
    modeStr = (powerMgr.currentMode == BATTERY_MODE) ? "Battery" : "AC Power";
    if (forcedACByCommFail) {
        modeStr += " <span style='color:red;'>(Comm Fault Recovery)</span>";
    }
    if (batteryModeTargetTime > 0) {
        unsigned long remaining = (SWITCH_CONFIRM_PERIOD - (millis() - batteryModeTargetTime)) / 1000;
        modeStr += " [Switching in " + String(remaining) + "s]";
    }
    
    html.replace("{{mode}}", modeStr);
    html.replace("{{time}}", getTimeAgo(powerMgr.lastModeChangeTime));
    html.replace("{{count}}", String(powerMgr.modeChangeCount));
    html.replace("{{comm_fail}}", String(bmsCommFailCount));
    html.replace("{{comm_total}}", String(bmsCommTotalFailCount));
    html.replace("{{restart}}", String(restartCount));
    
    server.send(200, "text/html", html);
});

  // 路由：Prometheus 指标端点
  server.on("/metrics", HTTP_GET, [](){
    String metrics = generatePrometheusMetrics();
    server.sendHeader("Connection", "close");
    server.sendHeader("Content-Type", "text/plain; charset=utf-8");
    server.send(200, "text/plain", metrics);
  });

  // OTA 上传处理：GET 提供页面，POST 处理文件上传并写入 flash
  server.on("/update", HTTP_GET, [](){
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", updateForm);
  });

  server.on("/update", HTTP_POST, [](){
    // 完成后返回结果并重启（如无错误）
    if (Update.hasError()) {
      server.send(200, "text/plain", "FAIL");
    } else {
      server.send(200, "text/plain", "OK");
    }
    delay(100);
    ESP.restart();
  },
  [](){ // upload handler
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      // Serial.printf("Update Start: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = 0;
  #if defined(ESP32)
      maxSketchSpace = (ESP.getFreeSketchSpace());
  #else
      maxSketchSpace = ESP.getFreeSketchSpace();
  #endif
      if (!Update.begin(maxSketchSpace)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(false)) {
        Update.printError(Serial);
        // Serial.printf("Update Success: %u bytes\n", upload.totalSize);
      } 
      // else {
      //   Update.printError(Serial);
      // }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      Update.end();
      // Serial.println("Update was aborted");
    }
  });

  server.begin();
  // Serial.println("HTTP server started");

  // 初始化定时重启计时器
  lastRestartTime = millis();
}

void loop() {
  // 每次 loop 开始时获取当前时间，避免重复定义
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastBMSRead = 0;
  unsigned long now = millis();
  
  // 新增：处理 http 请求
  server.handleClient();
  // 喂看门狗，防止因长时间阻塞触发重启
  #if defined(ESP32)
    esp_task_wdt_reset();
  #else
    ESP.wdtFeed();
  #endif
  
  // ========== WiFi 非阻塞重连检查 ==========
  // 每 30s 检查一次 WiFi 状态，若断开则重新连接
  if (now - lastWiFiCheck >= WIFI_RECONNECT_INTERVAL) {
    lastWiFiCheck = now;
    
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnectedFlag = false;
      // 尝试重新连接：使用 WiFi.begin() 而非 reconnect()，确保即使无缓存也能重连
      WiFi.begin(ssid, password);
    } else {
        // WiFi 从断开变为已连接
      wifiConnectedFlag = true;
    }
  }

  // ========== BMS 数据采集与电源管理 =========="
  
  // 每 2000ms 读取一次 BMS 数据
  if (now - lastBMSRead >= 2000) {
    lastBMSRead = now;
    
    sendBMSRequest();
    delay(100); 

    uint8_t bmsBuffer[BMS_RESPONSE_LENGTH];
    if (readBMSData(bmsBuffer, BMS_RESPONSE_LENGTH)) {
      if (parseBMSData(bmsBuffer, BMS_RESPONSE_LENGTH)) {
          bmsCommFailCount = 0; // 成功通讯，清除计数
      } else {
        bmsCommFailCount++;
        bmsCommTotalFailCount++;
      }
    } else {
      bmsCommFailCount++;
      bmsCommTotalFailCount++;
    }

    // 统一执行电源管理逻辑（包含了对 bmsCommFailCount 的判断）
    updatePowerMode();

    // 检查定时重启
    // 1. 检查重启标记
    if (!pendingRestart && (now - lastRestartTime >= RESTART_INTERVAL)) {
        pendingRestart = true;
    }

    // 2. 执行重启判断
    if (pendingRestart) {
        // 必须同时满足：1.市电模式 2.没有在切换电池的2分钟观察期内
        if (powerMgr.currentMode == AC_POWER_MODE && batteryModeTargetTime == 0) {
            // 重启前最后喂一次狗，确保重启过程不被 WDT 干扰
            #if defined(ESP32)
                esp_task_wdt_reset();
            #else
                ESP.wdtFeed();
            #endif
            
            delay(500); // 稍微停一下让系统日志处理完
            ESP.restart();
        }
    }
  }
}