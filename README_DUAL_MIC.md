# 双 INMP441 可行性验证

此工程用于 ESP32-S3 + 两颗 INMP441 + MAX98357 的本地音频验证。参考了 `../ai_client` 的引脚分配和双麦共享总线方案。当前使用“长按录音、松开回放”，不连接 AI 服务。

此前实时监听时出现持续尖叫，符合声学反馈的表现：扬声器声音被麦克风拾取后再次播放。为验证正常人声，现将录音和回放分开，播放只读取已保存的 PCM。回放音量从 1.0 回退为 0.25，并增加 20ms 渐入、渐出。尚需上板确认实际听音效果；渐变本身不能替代声学回声消除。

## 接线

| 信号 | ESP32-S3 | 左麦 | 右麦 |
| --- | --- | --- | --- |
| 电源 | 3.3V | VDD | VDD |
| 地 | GND | GND | GND |
| 位时钟 | GPIO5 | SCK/BCLK | SCK/BCLK |
| 帧时钟 | GPIO4 | WS/LRCLK | WS/LRCLK |
| 采集数据 | GPIO6 | SD | SD |
| 声道选择 | — | L/R 接 GND | L/R 接 3.3V |

两颗麦克风的 SD 接到同一根 GPIO6 数据线上；依靠相反的 L/R 电平分别在左右时隙驱动 SD。L/R 不悬空。麦克风用 3.3V 供电。确认模块是否已有去耦电容和 SD 下拉；芯片手册给出每颗 0.1µF 去耦、共享 SD 线 100kΩ 下拉的接法。若模块引出 CHIPEN，保持高电平。

MAX98357 保持原引脚：BCLK=GPIO15、LRC=GPIO16、DIN=GPIO7，功放与 ESP32 共地，SD/EN 保持启用。左右输出时隙写入相同音频，可兼容模块的声道选择。BOOT 使用板载 GPIO0。

## 采集格式

- I2S1 主机接收，标准 Philips I2S，16kHz，左右两个 32 位时隙，位时钟 1.024MHz。
- INMP441 的 24 位有符号数据位于 32 位时隙高位；右移 16 位转换为 PCM16。
- 启动丢弃约 512ms 数据。统计使用转换后的原始 PCM，录音增加直流去除。当前 `SPK_VOLUME=0.25f`，启动时输出 `Record/replay output volume=0.25; no live microphone loopback`。
- 录音使用 96000 字节静态缓冲，最多保存 3 秒所选模式的 PCM16 单声道数据。满后等待松手回放，不自动重录。
- I2S0 主机发送，16kHz，16 位双声道，同一单声道信号复制到左右时隙。

## 操作

1. 上电后默认静音、LEFT 模式。串口 115200 波特率，每采集 16000 帧输出一组日志，正常约每秒一次。
2. 短按并松开 BOOT，依次切换 `LEFT -> RIGHT -> AVERAGE -> DIFFERENCE -> LEFT`。
3. 按住 BOOT 超过 400ms 后出现 `record START`，再开始说话。录音时 `record=1 play=0`，扬声器静音。松开后出现 `replay START ... play=1`，自动回放刚才的录音；结束出现 `replay END ... play=0`。长按松开不切换模式。
4. 最多录 3 秒，缓冲满后提示 `record buffer FULL`，松手才回放。回放期间不处理新按键和新录音，结束后丢弃 RX 中的旧数据并等待约 128ms 再继续。上电或复位时先松开 BOOT，避免进入下载模式。
5. 试听时保持供电和接线稳定，先使用低音量。

试听人声时，在 `record START` 后距离麦克风约 10～20cm 正常说话，松手后听回放，并保留录音期间的 L/R 电平日志。吹气形成的低频和气流扰动不能代替语音测试。若回放仍有持续尖叫、爆音或听不到人声，需要结合采集电平、功放供电、音频格式和接线继续排查。

`AVERAGE=(L+R)/2` 是直接平均，没有延时补偿。`DIFFERENCE=(L-R)/2` 只用于对比两路信号。平均和差分都不能直接视为降噪算法。

### BOOT 偶尔无响应时

从启动完成后开始，按住 BOOT 约 2 秒再松开，保留整个过程的日志。正常依次出现 `BOOT pressed`、约 400ms 后 `record START`，松手后出现 `BOOT released`、`replay START`，回放结束出现 `replay END`。回放中不读取麦克风，因而不会输出新的 L/R 电平统计；录音期间 `play=0` 是正常行为。

音频统计末尾还包含按键采样状态：

- `boot=0/1`：GPIO0 原始电平，0 表示低电平（按下），1 表示高电平。
- `pressed=1/0`：经过 30ms 消抖后的按下状态。
- `held_ms`：从按下被确认起持续的时间。稳定按下超过 400ms 才开始录音。
- `edges`：本统计窗口内采到的原始电平变化次数，按下和松开各产生一次变化；持续按住期间多次变化需要检查按键电气信号。

若持续按住时 `boot` 一直为 1，说明当前采样没有看到 GPIO0 被拉低，优先确认按的是 GPIO0 对应的 BOOT 键。若 `boot` 变化频繁且 `pressed` 无法稳定为 1，进一步检查按键、连线或同引脚占用。若 `pressed=1`、`held_ms` 超过 400 却没有 `record START`，保留该行及前后的录音事件日志。缓冲满时 `record=0 play=0` 表示等待松手。

这些日志用于区分问题发生在哪一层，不能单凭现有日志认定按键损坏。上电和复位时不要按住 BOOT。

## 实测步骤与判断

1. **保持静音，分别靠近两颗麦说话或轻擦各自附近。** 日志中的 L/R 电平应随对应麦克风的声源变化，LEFT/RIGHT 试听应对应到实物。两麦都会听到环境声，因此无需另一声道完全静音。若位置相反，先核对 L/R 接线。
2. **分别验证两路。** LEFT、RIGHT 均应有人声，避免只测平均后掩盖一路失效。连续固定值会输出警告；若一路明显异常，检查它的供电、L/R 电平、焊点和共用 SD 接线。
3. **安静环境观察底噪。** `rms` 是去除本统计窗口均值后的均方根，`peak` 是原始绝对峰值，`dc` 是原始均值，`clip` 是接近 PCM16 满幅的样本数。记录两路安静底噪，再在相同距离、音量下说同一句话。安静时电平低本身不能证明麦克风损坏。
4. **对称摆放并从两麦正前方说话。** AVERAGE 应保持可懂度；DIFFERENCE 在两路声源近似同相、同幅时通常会减弱。实际结果取决于位置、距离、反射和麦克风差异。`corr` 是零延时相关系数，`valid=0` 表示某一路变化太小，此时 corr 无判断意义。
5. **加入侧方噪声再比较。** 保持声源、距离和播放增益一致，对比 LEFT、RIGHT 和 AVERAGE 的人声与底噪。记录结果后再决定是否迁移到 AI 工程；此程序没有量化语音信噪比，也不会自动宣布降噪成功。

`same` 是两路样本完全相等的比例；持续 100% 会警告两路尚未得到独立验证。高相关或两路都有电平并不能单独证明两颗麦正常，必须结合独立声源刺激和分别试听。

## 构建和测试

在已导出 ESP-IDF 5.4 环境的终端、此工程根目录执行：

```powershell
idf.py -B build/dual-mic build
# 将 COMx 替换为实际连接此开发板的串口
idf.py -B build/dual-mic -p COMx flash monitor
```

使用 `build/dual-mic` 避免沿用原 `build` 下不一致的 CMake 缓存。目标保持 ESP32-S3。

本机 C 编译器可验证实际音频转换代码：

```powershell
gcc -std=c11 -O2 -Wall -Wextra -Werror tests/test_dual_mic_pcm.c -o build/test_dual_mic_pcm.exe
./build/test_dual_mic_pcm.exe
```

测试覆盖有符号数据转换、左右声道选择、平均/差分满幅、直流去除、同相信号差分抵消、大幅阶跃的饱和限幅，以及回放渐入渐出。

```powershell
python tests/test_boot_control.py
python tests/test_record_replay.py
```

按键测试重放实际控制代码；录放测试执行实际采集循环和 I2S 写入函数，验证实时采集阶段只输出静音、回放输出只来自保存的 PCM、录音缓冲不会越界。上述测试和编译均无法替代硬件采集、接线和声学效果实测。

## 与 AI 语音工程的关系

参考 `ai_client/components/BSP/I2S/i2s.h` 中的 `I2S_DUAL_MIC_NS_ENABLE` 当前为 0，注释记录了第二麦产噪和 VAD 不结束的现象。该工程的自适应步长也设为 0。

**2026-11-13 更新（AI 工程侧）**：AI-Safety-Inspection 已不再自己写双麦降噪，改为
把**两路原始信号**（通道交错，`receive_audio_stereo()`）直接喂给 esp-sr 的 AFE：
`afe_config_init("MM", ...)`。理由正是上面那条踩坑记录——手写差分/自适应会把目标
人声一起削掉，空间的事交给按双麦几何关系调过的算法。

按 esp-sr 官方 benchmark（ESP32-S3 段），双麦配置的实际流水线是
`AEC -> SE(BSS) -> VAD -> WakeNet`，**没有 NS**：`afe_config_check()` 的规则是
"双通道时 SE(BSS) 优先于 NS"，也就是 BSS 把 NS 顶掉。所以这条路买到的是
**空间分离**（目标声源 vs 其他方向的干扰），不是稳态噪声抑制；要压扩散性环境噪声
需要另挂 nsnet2（单通道，S3 支持，但本工程未启用）。

官方给出的代价：internal RAM 49.1 KB → 82.2 KB，PSRAM 775.8 KB → 1198.2 KB，
feed CPU 15.9% → 46.2%（单核），fetch CPU 15.7% → 32.4%。另注：官方的
GSC 波束形成、NN Beamforming 都只支持 ESP32-P4 / ESP32-S31，**不支持 S3**，
所以 S3 上双麦空间降噪只有 AFE 的 BSS 这一条路。

本工程的"分别验证两路电平"仍是上板前的必要步骤：BSS 要有效，两路必须都是
真实、独立、同步采样的信号，且两颗麦要有间距、目标声源在正前方。

本工程没有自适应降噪、波束形成或声学回声消除。若后续希望 AI 播报时仍可打断对话，还需评估播放参考信号和 AEC；两颗麦本身不保证这些能力。

参考：[INMP441 芯片手册](https://product.tdk.com/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf)、[ESP-IDF 5.4 ESP32-S3 I2S 文档](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/peripherals/i2s.html)。
