
# 派蒙智能ESP32门锁

## 项目简介
派蒙智能门锁是一款基于 ESP32 的蓝牙智能门锁系统，旨在提供安全、便捷的门锁控制体验。用户可以通过蓝牙连接设备，输入密码以解锁门锁。系统还支持多种 BLE 功能和实时通知。

## 功能特性
1. **蓝牙连接**
   - 支持 BLE（Bluetooth Low Energy）连接。
   - 设备名称：`ESP32_BLE_LOCK`。

2. **密码验证**
   - 用户通过 BLE 输入密码。
   - 验证密码是否正确，正确则开门。

3. **门锁控制**
   - 使用伺服电机模拟门锁开关。
   - 开门后自动复位。

4. **实时通知**
   - 密码验证结果通过 BLE 通知客户端。
   - 提供欢迎消息和错误提示。

5. **LED PWM 控制**
   - 使用 LEDC 模块控制伺服电机。
   - 支持 PWM 占空比调节。

6. **多连接支持**
   - 支持多个客户端连接。
   - 连接状态实时更新。

## 系统架构
- **硬件**
  - ESP32 开发板
  - 伺服电机
  - GPIO 接口

- **软件**
  - 基于 ESP-IDF 框架开发
  - 使用 NimBLE 实现 BLE 功能
  - FreeRTOS 任务管理

## 使用方法
1. **初始化设备**
   - 上电后，设备会自动初始化并开始广播。
   - 使用 BLE 扫描工具连接设备。

2. **输入密码**
   - 连接成功后，设备会发送欢迎消息。
   - 在客户端输入密码并发送。

3. **验证结果**
   - 如果密码正确，门锁会打开并发送成功消息。
   - 如果密码错误，设备会提示重新输入。

4. **断开连接**
   - 完成操作后，客户端可以断开连接。

## 开发环境
- **工具链**
  - ESP-IDF v5.4.1
  - CMake

- **依赖**
  - NimBLE 库
  - FreeRTOS

## 未来计划
- 增加WiFi连接功能。
- 实现动态密码功能。
- 增加日志记录和远程管理功能。

## 贡献
欢迎对本项目提出建议或贡献代码！
