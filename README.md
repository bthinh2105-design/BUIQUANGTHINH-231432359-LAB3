# LAB 3 – ĐỀ 3: FIRE ALARM SYSTEM

## 1. Giới thiệu đề tài

Thiết kế hệ thống báo cháy gia đình sử dụng ESP32, cảm biến MQ-2 và DHT22. Hệ thống có chức năng phát hiện khí gas/khói, giám sát nhiệt độ, cảnh báo tự động và điều khiển thiết bị thông qua MQTT Broker.

## 2. Công nghệ sử dụng

- ESP32 DevKit
- Cảm biến khí gas MQ-2
- Cảm biến nhiệt độ DHT22
- FreeRTOS
- MQTT Protocol
- HiveMQ Public Broker
- HTML, CSS, JavaScript
- MQTT.js và Chart.js
- Wokwi Simulator


## 3. Các Task FreeRTOS

- TaskReadData – Core 1: Đọc cảm biến và đưa dữ liệu vào Queue.
- TaskSendData – Core 0: Kết nối Wi-Fi, MQTT và gửi dữ liệu.
- TaskAlert – Core 1: Xử lý cảnh báo và điều khiển thiết bị.

Các Queue sử dụng: dataQueue, dataAlertQueue và cmdQueue.

## 4. Ngưỡng điều khiển

- Gas ADC > 1800: Bật quạt.
- Nhiệt độ >= 50°C: Bật máy bơm.
- Hỗ trợ hai chế độ AUTO và MANUAL.

## 5. MQTT Broker

Broker: broker.hivemq.com

ESP32 MQTT TCP Port: 1883

WebSocket Port: 8000


## 8. Link Wokwi
(https://wokwi.com/projects/480580487536267265)
