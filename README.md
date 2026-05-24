# Chương II: Tính năng hệ thống

---

## 2.1 Tổng quan tính năng

Hệ thống SmartLock được xây dựng trên nền tảng ESP32-S3, tích hợp đồng thời nhiều phương thức xác thực, giao diện vật lý và giao diện web, cùng các cơ chế bảo mật nhiều lớp. Bảng dưới liệt kê tổng quan các nhóm tính năng chính:

| Nhóm | Tính năng |
|------|-----------|
| Xác thực | PIN Scramble, Thẻ RFID/NFC, OTP qua email |
| Bảo mật | Khóa tạm, Khóa thẻ, Vô hiệu hóa hệ thống, Cảnh báo giả mạo cửa, Phân quyền & khung giờ |
| Giao diện TFT | Màn hình khóa, Nhập PIN, Quét thẻ, WiFi, Quản lý người dùng & thẻ |
| Web Dashboard | Điều khiển từ xa, Quản lý user/thẻ, Log truy cập, SMTP, Đăng nhập bằng thẻ |
| Thông báo | Email cảnh báo tự động, OTP qua email |
| Kết nối & thời gian | WiFi AP mode tự cấu hình, NTP + RTC DS3231 |
| Lưu trữ | NVS Flash, RTC_DATA_ATTR, SPIFFS |
| Năng lượng | Backlight timeout, Light sleep, Wake-up khẩn cấp |

---

## 2.2 Xác thực đa phương thức

### 2.2.1 PIN Scramble

Bàn phím 10 nút hiển thị trên màn hình TFT. Thay vì nhập đúng PIN, người dùng nhập một chuỗi dài hơn — hệ thống chấp nhận nếu PIN thực **là substring** của chuỗi nhập vào. Cơ chế này chống lại tấn công quan sát, vì thứ tự và vị trí bấm thay đổi mỗi lần.


### 2.2.2 Thẻ RFID/NFC (PN532)

Mỗi user được gắn tối đa 1 thẻ. Khi quẹt thẻ hợp lệ, hệ thống xác thực ngay lập tức mà không cần nhập PIN. Quản lý thẻ (gắn/xóa) thực hiện được trên cả **TFT** lẫn **Web UI**.

Cơ chế **khóa thẻ**: quẹt thẻ không hợp lệ liên tiếp `MAX_PASSWORD_ATTEMPTS` lần → chức năng thẻ bị vô hiệu hóa toàn bộ, màn hình hiển thị cảnh báo. Admin reset qua web (`/api/reset-card-lock`).

---

## 2.3 Cơ chế bảo mật

### 2.3.1 Khóa tạm theo thời gian (Password Lockout)

Sau `MAX_PASSWORD_ATTEMPTS` lần nhập sai PIN liên tiếp, hệ thống chuyển sang `STATE_PASSWORD_LOCKOUT`. Thời gian chờ tăng dần theo cơ chế **exponential backoff** — mỗi lần lockout tiếp theo thời gian dài hơn. Countdown hiển thị trực tiếp trên màn hình TFT.


### 2.3.2 Vô hiệu hóa hệ thống (System Disabled)

Sau tổng cộng `MAX_TOTAL_ATTEMPTS` lần xác thực thất bại (tích lũy qua nhiều phiên lockout), hệ thống chuyển sang `STATE_SYSTEM_DISABLED` — không còn chấp nhận bất kỳ xác thực nào. Phục hồi chỉ bằng lệnh reset từ web UI (cần đăng nhập admin) hoặc factory reset vật lý (giữ nút 5 giây).

Email cảnh báo được gửi tự động khi trạng thái này kích hoạt.

### 2.3.3 Cảnh báo giả mạo cửa (Door Tamper Alarm)

Cảm biến từ MC38 giám sát trạng thái cửa. Nếu cửa bị mở **khi hệ thống đang khóa** → chuyển `STATE_ALARM`, còi hú, backlight bật ngay lập tức (kể cả khi đang light sleep), email cảnh báo được gửi đi. Admin xác nhận (acknowledge) qua web để tắt alarm.


### 2.3.4 Phân quyền & kiểm soát khung giờ

Mỗi user có `role`: `Admin` / `User` / `Guest`. Ngoài ra, mỗi user có cấu hình `allowedStart` / `allowedEnd` (ví dụ `07:00`–`18:00`). Nếu xác thực diễn ra **ngoài khung giờ**, hệ thống từ chối dù PIN/thẻ đúng. Admin không bị giới hạn giờ.

---

## 2.4 Giao diện TFT (LVGL)

Giao diện được thiết kế bằng **SquareLine Studio** và render bằng thư viện **LVGL 8.3** trên màn hình ILI9488 320×480, cảm ứng điện trở XPT2046.

**Sơ đồ điều hướng màn hình:**

```
[Boot] → [Màn hình khóa - uilock]
              ├── Tap "Enter PIN" → [Bàn phím Scramble - uiT9]
              │       ├── Đúng → [Mở khóa thành công - uisuccess] → [uilock]
              │       └── Sai  → [Cảnh báo + countdown 3s - uiwrongpin] → [uilock]
              ├── Quẹt thẻ (tự động)
              └── Cài đặt (admin PIN)
                    ├── [Quét & kết nối WiFi - uiWifi]
                    └── [Quản lý người dùng - uiSelectUser]
                              ├── Gắn thẻ → [Quét thẻ - uiScanCard]
                              ├── Xóa thẻ
                              ├── Thêm user mới
                              └── Xóa user
```


---

## 2.5 Web Dashboard


---

## 2.6 Thông báo Email

Gửi HTML email qua SMTP (mặc định Gmail port 465, implicit TLS) sử dụng thư viện **ReadyMail** với `WiFiClientSecure`.

| Sự kiện | Nội dung email |
|---------|---------------|
| Hệ thống bị vô hiệu hóa | Cảnh báo, IP, thời gian |
| Cửa bị giả mạo | Cảnh báo alarm, trạng thái |
| OTP request | Mã 6 số, hạn dùng 5 phút |

Cấu hình SMTP (server, port, tài khoản, mật khẩu) lưu trong NVS, có thể chỉnh và test thử gửi trực tiếp từ web UI.

---

## 2.7 Quản lý kết nối & thời gian

**WiFi:** Lần đầu chưa có cấu hình → ESP32 mở AP mode, người dùng kết nối và cấu hình qua captive portal (WiFiManager). Sau đó tự kết nối lại khi khởi động.

**NTP + RTC DS3231:** Khi WiFi kết nối, hệ thống đợi ~5 giây để SNTP nhận response từ `pool.ntp.org`, sau đó ghi thời gian vào RTC DS3231 (I2C, pin CMOS backup). Định kỳ re-sync mỗi 6 giờ. Khi mất WiFi, thời gian vẫn chính xác từ DS3231 (sai số < 2 ppm).

---

## 2.8 Lưu trữ dữ liệu

Hệ thống sử dụng 3 tầng lưu trữ với mục đích khác nhau:

| Tầng | Công nghệ | Dữ liệu | Đặc điểm |
|------|-----------|---------|----------|
| NVS | ESP32 Preferences (Flash) | Users, mật khẩu, settings, SMTP, logs | Bền vững qua reboot, power-off |
| RTC_DATA_ATTR | SRAM của RTC domain | Fail count, lockout time, system disabled | Sống qua **deep sleep**, mất khi flash lại |
| SPIFFS | Flash partition riêng | HTML/JS/CSS Web UI | Read-only, cập nhật khi OTA/flash |

Phân vùng Flash được tùy chỉnh (partitions.csv): ~3 MB cho firmware, ~1.5 MB cho SPIFFS web assets.

---

## 2.9 Quản lý năng lượng

| Cơ chế | Mô tả |
|--------|-------|
| **Backlight timeout** | Tắt đèn màn hình sau N giây không hoạt động (cấu hình: 5s–120s). Chạm màn hình hoặc quẹt thẻ → bật lại ngay lập tức. |
| **Light sleep** | Sau 5 phút không hoạt động, ESP32 vào light sleep 30 giây rồi tỉnh kiểm tra, lặp lại → giảm tiêu thụ điện ~75% so với luôn hoạt động. |
| **Wake-up khẩn cấp** | Nếu cảm biến cửa kích hoạt trong khi đang ngủ (EXT1 wakeup) → tỉnh ngay, bật backlight, chuyển STATE_ALARM, không chờ người dùng tương tác. |
