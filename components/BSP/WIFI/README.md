# BSP/WIFI — STA 连接 + SoftAP 网页配网

## 文件

| 文件 | 职责 |
|---|---|
| `wifi.c/.h` | WiFi 主状态循环：连接、指数退避重试、保活轮询配网请求 |
| `wifi_provision.c/.h` | SoftAP captive portal 配网：NVS 凭证 + DNS 劫持 + httpd 配网页 |
| `portal.html` | 配网页面（CMake `EMBED_TXTFILES` 内嵌进固件，改动后需重新编译） |

## 配网流程

1. **首次上电**（NVS 无凭证）或**长按 BOOT(GPIO0) ≥3 秒** → 进入配网模式
2. 开放热点 `ESP配网服务`，LED 快闪
3. 手机连接热点后自动弹出配网页（或浏览器访问 `http://192.168.4.1`）
4. 网页点选扫描到的 WiFi（或手输 SSID）+ 密码 → 保存 NVS → STA 试连
   - 成功：页面提示连接成功，3 秒后自动关闭热点，LED 常亮
   - 失败：页面提示错误，可直接改密码重试（热点不中断）
5. 之后每次上电直接用 NVS 凭证连接，不再进配网

## 关键设计

- 凭证存 NVS 命名空间 `wifi_prov`（键 `ssid`/`pass`）；驱动侧
  `esp_wifi_set_storage(WIFI_STORAGE_RAM)`，不再隐式落盘旧配置
- 所有 `esp_wifi` 模式切换集中在 wifi 任务；长按/网页只置请求或调
  `wifi_sta_apply_credentials()`，不直接切模式
- 断连重试：指数退避 1s→2s→…→30s 封顶，**无限重试**（取代旧版
  "100 次后置 FAIL 位永久放弃"）
- 配网试连在 APSTA 下进行，portal 保持在线；`GOT_IP` 钩子触发门户
  延时 3s 自动收尾（给成功页留刷新时间）
- 手机 captive portal 弹窗：UDP:53 DNS 劫持（全域名→本机）+ 404 统一
  302 回首页

## 已知限制

- **开发期重新配网（BOOT 键硬件损坏期间）**：擦 NVS 清凭证，重启即自动进配网：
  `python -m esptool --chip esp32s3 -p /dev/ttyUSB0 erase_region 0x9000 0xf000`
  （现场部署前需修复 BOOT 键，否则换环境/改密码无法重配）
- 重配网成功后 ESP-NOW 不会自动恢复（其任务 30s 超时自删，重启恢复）；
  如需要可在 GOT_IP 处挂恢复钩子
- 连接失败**不会**自动进入配网（避免路由器短暂断网误触发），需长按 BOOT 手动重配
- 服务器地址/设备密钥仍为编译期宏（`AI_CHAT/my_client.h`），后续可用同一套
  NVS 模式做可配置化
- 配网期间 portal 也监听 STA 网段（联网状态下长按触发时），仅短暂存在
