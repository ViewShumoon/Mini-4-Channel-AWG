# ESP32-C3 ←BLE(coyote/v3)→ SPI → Tang Nano 9K 链路设计

范围：**仅覆盖 "ESP32 接收蓝牙波形描述 → SPI 传给 FPGA" 这一段**。
上端的手机 APP 与下端的 DDS/DAC 模拟后端不在本文范围内，本文只给出 FPGA 侧必须满足的接口契约。

## 0. 决策基线

| 项 | 决定 | 后果 |
| --- | --- | --- |
| 波形协议 | `dglab-bluetooth-protocol/coyote/v3`，**不新增私有指令** | 只有 A/B 两通道可用；本工程 **仅 CH0=A、CH1=B**，CH2/CH3 不由本通路驱动 |
| 25ms 时间片展开 | **ESP32 侧定时逐格推送** | FPGA 不实现播放表与槽位时基，只暴露"当前生效"寄存器；波形节拍受 RTOS 抖动影响 |
| ESP32-C3 的 BLE 角色 | **Peripheral**（扮演"脉冲主机 3.0"，被手机写入） | 必须复刻 v3 的 GATT 表与广播名，才能被现有 APP 识别 |
| 固件框架 | Arduino / PlatformIO (`esp32` core) | 用 `SPI` 库 + `NimBLE`/`BLEDevice`，硬件 SPI 走 GPIO 矩阵 |
| DDS 波形表 | **周期归一化**（表 = 1 个脉冲周期） | 改频只改 `phase_inc`，不重载波形表；脉宽随 1/f 缩放（占空比恒定） |
| SPI 链路 | 自定义 4 类帧：Type-R（单寄存器）/ **Type-G（多寄存器原子合并写，生产路径）** / Type-W（波形单点）/ Type-B（波形 Burst），CRC-8 校验 | 每 25 ms 槽仅 1 次 CS# 事务、24 B；帧长由 CS# 定界（§6.2） |
| DDS 工作时钟 | **27 MHz 直用板载 `clk27`，不引入 PLL**（§9 I2） | 频率分辨率 6.29 mHz、4096 点表基音上限 6.59 kHz；`F_CLK_DDS` 是两侧唯一换算常量 |
| 波形表上电内容 | **预置单极脉冲（12.5% 占空比）**（§9 I7） | 烧录后不必先下载波形即可见输出；仍需下发 `phase_inc` 累加器才启动 |
| DAC 型号 | **2× MCP4822（12-bit，SPI）**（§9 I3） | 与 README 一致；`__init__.py` 里的 MCP4728/I2C 描述已清理，16-bit 仅为 `data_width` 预留 |

## 1. 数据流与职责划分

```text
手机 APP (Central)
   │  BLE GATT Write Without Response → 0x180C/0x150A   每 100ms 一条 B0 (20B) / 一次 BF (7B)
   ▼
ESP32-C3 (Peripheral)
   ① GATT 服务与 Notify
   ② B0/BF 解码 + 取值域校验（整条原子判定）
   ③ 强度合成（通道强度 × 波形强度 × 软上限）
   ④ 100ms 序列 → 25ms 槽位展开（本地定时器）
   ⑤ 频率刻度逆映射 + phase_inc 计算
   ⑥ SPI Master 帧封装
   │  CS/SCLK/MOSI/MISO/IRQ  (5 线, 3.3V)
   ▼
Tang Nano 9K (GW1NR-9C, Amaranth)
   ⑦ SpiSlave 帧解析 → ⑧ RegFile → ⑨ Watchdog
                                ├→ DdsTop.phase_inc[] / amp_scale[]
                                └→ DdsTop.spi_w_*（波形表写入通路）
   ▼
   4 路 dac_out → DAC Driver → 2×MCP4822 → AFE
```

ESP32 是**唯一的实时性责任方**：它必须保证每 25ms 交付一格新参数，并在 BLE 异常时立刻静默。FPGA 是**纯参数消费者**，无自有波形时基（除看门狗外）。

## 2. BLE 外设侧

### 2.1 GATT 表（必须与 v3 完全一致）

基础 UUID `0000xxxx-0000-1000-8000-00805f9b34fb`。

| 服务 | 特征 | 属性 | 最大长度 | 用途 |
| --- | --- | --- | --- | --- |
| `0x180C` | `0x150A` | WRITE | 20 B | 所有下行指令（`0xB0`/`0xBF`） |
| `0x180C` | `0x150B` | NOTIFY | 20 B | 所有上行回应（`0xB1`） |
| `0x180A` | `0x1500` | READ | 1 B | 电量（固定回报 `0x64` = 100%，无电池时给常量即可） |

广播名必须为 **`47L121000`**（"脉冲主机 3.0"）。APP 侧通常按名字/前缀过滤扫描结果，名字不对会导致设备根本不出现在列表中。

### 2.2 B0 帧位级布局（20 字节）

```text
idx  字段                       位/长度
 0   HEAD = 0xB0                1 B
 1   [7:4] seq 序列号            4 b   (0..15)
     [3:0] method 强度解读方式    4 b   高 2 b = A 通道, 低 2 b = B 通道
 2   strength_A 设定值           1 B   (0..200)
 3   strength_B 设定值           1 B   (0..200)
 4..7   freq_A  [slot0..slot3]   4 B   (10..240)
 8..11  amp_A   [slot0..slot3]   4 B   (0..100)
12..15  freq_B  [slot0..slot3]   4 B   (10..240)
16..19  amp_B   [slot0..slot3]   4 B   (0..100)
```

无校验和字段——**BLE 链路层 CRC 即为其校验**，ESP32 侧只做取值域检查，不做端到端 CRC。这与本工程 SPI 段必须自带 CRC 形成对比（§6.3）。

### 2.3 `method` 语义与通道强度状态机

`method` 高 2 位控制 A、低 2 位控制 B；每个通道的 2 bit 取值：

| 值 | 含义 | 运算 |
| --- | --- | --- |
| `0b00` | 不改变 | 忽略该通道设定值 |
| `0b01` | 相对增加 | `s = min(s + v, soft)` |
| `0b10` | 相对减少 | `s = max(s - v, 0)` |
| `0b11` | 绝对设定 | `s = clamp(v, 0, soft)` |

设定值超出 `0..200` 时按 `0` 处理（协议例 3/4：`v=201` 且 `method=0b01` → 强度保持 10；`method=0b11` → 强度归 0）。因此合法增量必须先做 `v > 200 ? 0 : v` 归一。

**序列号（`seq`）状态机**——用于把 `B1` 回告与触发它的那条 `B0` 配对：

```text
seq != 0  → 本条 B0 修改了通道强度，处理完后必须 Notify 一条 B1，且 B1.seq = 本条 B0.seq
seq == 0  → 不回应 B1（即使强度已变化）
本地（旋钮/按键等效事件）引起强度变化 → B1.seq = 0
```

ESP32 无旋钮，但必须实现同一契约，否则 APP 的 `isInputAllowed` 门控（协议例 1→16 序列）会永久卡在 `false`，表现为"强度调不上去"。实现要求：
- 维护 `pending_seq`；`B1` 发出后清 `pending_seq`。
- 同一 `seq` 的 `B1` 未回告前，若 APP 又发来新 `seq != 0` 的 B0，仍按新命令执行并回告新 `seq`（协议未禁止乱序，但 APP 建议等待）。

### 2.4 BF 帧（7 字节）与断电保存

```text
0  HEAD = 0xBF
1  soft_A     0..200    2  soft_B
3  bal1_A     0..255    4  bal1_B     频率平衡参数 1（冲击感）
5  bal2_A     0..255    6  bal2_B     强度平衡参数 2（脉宽）
```

- BF **写入即生效、无回告**，且协议明确要求"每次重连后必须重新写入"。
- 超范围值不修改对应字段（保留旧值），且**不返回任何错误**。
- `soft_*` / `bal1_*` / `bal2_*` 在原机上断电保存。ESP32-C3 侧用 **NVS**（`Preferences`/`nvs_flash`）持久化，键名 `awg.softA/softB/bal1A/...`；首次上电无记录时取默认 `soft=200, bal1=128, bal2=128`。

`bal1/bal2` 在郊狼上是私有标定曲线，v3 未公布其函数形式。本设计的处理：
- `bal1/bal2` 照常接收、持久化、并在诊断寄存器中回读；
- **默认不参与幅度计算**（`CFG_USE_BAL = 0`）。
- 若后续需要近似，用可配置的一阶修正 `amp_eff = amp × (1 + k1×(bal1-128)/128 × (1 - f/f_max))`，`k1` 为编译期常量。
> 标记为**未知项**（§11 Q3），不做臆测标定。

### 2.5 取值域校验与"整通道原子丢弃"

协议原文：*若某通道的输入值不在有效范围，则脉冲主机会放弃掉该通道**全部 4 组**数据*。示例 No.1 正是用 `amp_B[3] = 101` 让 B 通道全程静默，从而实现"只输出 A"。

因此校验**必须先于任何写入**，逐通道独立：

```text
valid_A = all(10 <= freq_A[i] <= 240 for i in 0..3) and all(0 <= amp_A[i] <= 100 for i in 0..3)
valid_B = 同上
```

`valid_X == false` 时的行为，本设计取**该通道本 100ms 静默**（`amp_scale=0`, `EN=0`），而不是沿用上一序列尾部——"放弃全部 4 组"字面即不产生输出，静默也是最安全的解释。
校验与提交严格分离：先算出两通道的完整 4 槽结果，再一次性装载到 ESP32 的发送排程器，避免半更新。

## 3. v3 语义 → AWG 物理量映射

### 3.1 频率刻度逆映射（线上字节 → Hz）

v3 线上字节是压缩刻度：APP 先在 `10..1000` 的人机刻度上分段编码后发送，主机收到 `10..240`。

| 线上字节 `w` | 逆映射后频率 (Hz) | 来源分段 |
| --- | --- | --- |
| `10 .. 100` | `w` | `in 10..100 -> w` |
| `101 .. 200` | `(w - 100) * 5 + 100` | `in 101..600` 段 |
| `201 .. 240` | `(w - 200) * 10 + 600` | `in 601..1000` 段 |

边界说明：正向编码在 `w=100`（原值 100 与 101..104）与 `w=200`（原值 600 与 601..609）处有重叠，逆映射统一取**区间下界**，误差不超过 4 Hz / 9 Hz，且在刺激器精度下无意义。

提供编译开关 `AWG_FREQ_DIRECT`：
- `=1`（默认，行为对齐真机）：直接把 `w` 当 Hz，`10..240 Hz`。
- `=0`：用上表逆映射，得 `10..1000 Hz`。

### 3.2 `phase_inc` 计算

```text
phase_inc = round(f_pulse_Hz × 2^32 / f_clk_dds)
```

**频率分辨率** = `f_clk_dds / 2^32`。**清单 I2 已定稿：`f_clk_dds = 27 MHz`**——直用平台 `default_clk = clk27`（PIN 52），不引入 PLL。数值单一来源为 `awg_platform.F_CLK_DDS = 27_000_000`，ESP32 侧必须使用同一常量，否则输出频率整体偏移。备选频点一并列出，仅在将来确实需要更高重放率时改选：

| f_clk_dds | 分辨率 | 最大理论重放率 (4096 点) |
| --- | --- | --- |
| **27 MHz（定稿：板载直用，零风险）** | 6.29 mHz | 6.59 kHz 表基音 |
| 50 MHz（需 PLL，27→50 非整数比，需 VCO≈1000 MHz 或改选频点） | 11.6 mHz | 12.2 kHz |
| 108 MHz（需 PLL，27 MHz ×4 整数比） | 25.2 mHz | 26.4 kHz |

`phase_inc` 对照（`AWG_FREQ_DIRECT=1`，`f_out = w`）——**本设计取下表 `@27 MHz` 列**：

`f_clk_dds` 越大，同一 `f_out` 所需 `phase_inc` 越小（108 MHz 列 = 27 MHz 列 ÷ 4）。

| 线上 `w` (Hz) | @27 MHz | @50 MHz | @108 MHz |
| --- | --- | --- | --- |
| 10 | 1591 | 859 | 398 |
| 20 | 3181 | 1718 | 795 |
| 50 | 7954 | 4295 | 1988 |
| 100 | 15907 | 8590 | 3977 |
| 200 | 31815 | 17180 | 7954 |
| 240 | 38177 | 20616 | 9544 |

ESP32 用 64 位整数避免浮点：
`phase_inc = (uint32_t)(((uint64_t)f * 4294967296ULL + fclk/2) / fclk);`

### 3.3 强度合成（A/B 通道共用公式）

三级量：`通道强度 s`（0..200，全局）→ `软上限 soft`（0..200）→ `波形强度 a`（0..100，逐 25ms 槽）。

```text
s_eff   = min(s, soft)                      # 软上限裁剪
gain    = (s_eff / 200) × (a / 100)         # 0.0 .. 1.0
amp12   = round(gain × 4095)                # 送入 DdsChannel.amp_scale
```

例（`soft=150`）：

| `s` | `a` | `s_eff` | `gain` | `amp12` |
| --- | --- | --- | --- | --- |
| 100 | 50 | 100 | 0.250 | 1024 |
| 200 | 100 | 150 | 0.750 | 3071 |
| 100 | 101 | — | — | **通道静默**（§2.5） |
| 30 | 100 | 30 | 0.150 | 614 |

`amp12` 直接对接 [dds_channel.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_channel.py) 的 `raw_product = r_port.data * amp_scale; dac_out = raw_product[12:]`，即 `dac_out = (raw × amp12) / 4096`，是**纯线性增益**。

> ⚠ 该增益不扣除直流偏置。若波形表以 2048 为"零电平"（正弦/三角），则 `amp_scale` 会同时缩小偏置，输出向 0 而非向 2048 收拢。v3 的电脉冲基线为 0，**当前映射正确**；若将来要输出居中正弦，必须在 DDS 输出级增加 `(raw-2048)×scale+2048` 的双向缩放（§9 I5）。

### 3.4 波形表模型

采用**周期归一化**：4096 点表覆盖 1 个脉冲周期，改频仅改 `phase_inc`。
代价：脉冲**绝对**宽度随频率反比缩放。表内 8 点在 `w=10`（周期 100 ms）时占 8/4096×100 ms ≈ **195 µs**；在 `w=240`（周期 4.17 ms）时同样 8 点只占 ≈ **8.1 µs**。若刺激器要求脉宽恒定，此模型不成立。

若需恒定绝对脉宽，则每次换频都要重载波形表。单点单帧不可行（4096 × 8 B = 32768 B = 262144 bit，@4 MHz ≈ 65.5 ms > 25 ms 槽），必须用 §6.2 的 `WAVE_BURST`：纯数据 8192 B @4 MHz ≈ 16.4 ms，可塞进一个槽。**本版本不启用**（`AWG_FREQ_DIRECT` 下脉宽语义非必需），但接口与命令已预留。

## 4. 25ms 槽位排程器（ESP32）

```text
状态: slot_idx ∈ 0..3, sched[2][4] = {phase_inc, amp12, enable}
触发:
  - 收到有效 B0 → 校验(§2.5) → 计算 sched → slot_idx = 0 → 立即推送 slot0 → esp_timer 起 25 ms
  - esp_timer 到期 → slot_idx++ → 推送 sched[slot_idx]；slot_idx==4 → 进入 IDLE 并静默
  - 100 ms 内未收到新 B0 → 判定 BLE 流中断 → 静默(§8)
```

设计要点：
1. **以 B0 到达沿为槽 0 起点**，后续 3 槽由 `esp_timer`（或 FreeRTOS period timer）驱动。BLE 连接间隔抖动只影响首槽，不影响 4 槽内部节拍。
2. **4 槽播完即静默**，不做循环重播。v3 要求每 100 ms 刷新，缺失即视为失控。
3. 每槽一次 SPI 事务（单次 CS# 拉低），用 §6.2 的 **Type-G 合并写**一次提交 CH0/CH1 的 `phase_inc` + `amp_scale` 共 4 个寄存器。
4. SPI 占用时长：Type-G `COUNT=4` → 24 B @4 MHz = 48 µs；退化为 4 个独立 Type-R 帧 → 32 B = 64 µs。均 **< 槽预算 25 ms 的 0.3%**，BLE 栈与 SPI 无冲突。
5. `amp_scale` 与 `phase_inc` 写在同一帧内 → 无"新频率 × 旧幅度"的中间态。
6. 推送失败（`R_REG32` 回读 ID 不符或 `STATUS.CRC_ERR` 置位）→ 重试 1 次，仍失败则立即静默并停机等待人工复位。`IRQ#` 方向为 FPGA→ESP32，ESP32 不能主动驱动它。

抖动来源与量级（Arduino/`esp32` core）：BLE 栈中断优先级高于普通 task，`esp_timer` 回调抖动典型 < 200 µs，但 Wi-Fi/BT 共存或 GC 场景可达 ms 级。这是"ESP32 逐格推送"方案的固有代价，需在实测中确认是否可接受（§11 Q1）。

## 5. 引脚与电气

沿用 [awg_platform.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/awg_platform.py) 中 `AwgPlatform` 登记的 `esp32_spi` 资源（复用 RGB LCD 引脚，不用屏时可用；原 `temp_pins.py` 草稿已删除，清单 I1）：

| 信号 | FPGA 引脚 | 方向(FPGA) | ESP32-C3 | 说明 |
| --- | --- | --- | --- | --- |
| SCLK | PIN 33 (IOB23A) | in | GPIO 6 | 空闲低，SPI Mode 0 |
| CS# | PIN 34 (IOB23B) | in | GPIO 10 | 低有效，软件控制 |
| MOSI | PIN 35 (IOB29A, GCLKT_4) | in | GPIO 7 | FPGA 收 |
| MISO | PIN 40 (IOB33B) | out | GPIO 5 | FPGA 发（回读/状态） |
| IRQ# | PIN 41 (IOB41A) | out | GPIO 4 | FPGA→MCU，低有效开漏 + 上拉 |
| GND | — | — | GND | **必须共地** |

两侧均为 3.3V 逻辑，可直接对接。建议：
- 全部信号线串 33–100 Ω，线长 < 15 cm；CS#/SCLK 优先靠近 FPGA 侧。
- FPGA 各 3.3V 电源脚旁 0.1 µF；ESP32 侧另加 10 µF（BLE 突发电流）。
- `IRQ#` 与 `MISO` 必须配 `IO_TYPE="LVCMOS33"` 且 `PULL_MODE="UP"`（`IRQ#` 为开漏输出）。
- 未使用的 PIN 42 (IOB41B) 留作第二 IRQ 或 SPI 打点观测。
- **PIN 35 占用 `GCLKT_4`**。本设计的 SCLK 走**过采样**而非真实时钟树，故不强制需要 GCLK；若后续改源同步方案，应把 `sclk` 与 `mosi` 对调（SCLK→PIN 35）。
- ESP32-C3 侧 GPIO 4/5/6/7/10 均为通用脚，避开 strapping 脚（GPIO 2/8/9）与 flash 脚（GPIO 11–17）。`SPI.begin(sck=6, miso=5, mosi=7, cs=10)` 经 GPIO 矩阵映射，IRQ 用 `pinMode(4, INPUT_PULLUP)`。

FPGA → DAC 侧使用同一文件中登记的 `dac_spi`（2× MCP4822，SCLK/SDI/LDAC 并联共享、每片独立 CS，与 README 一致，清单 I3）：

| 信号 | FPGA 引脚 | 方向(FPGA) | 说明 |
| --- | --- | --- | --- |
| SCLK | PIN 25 | out | 两片并联 |
| SDI | PIN 26 | out | 两片并联（MCP4822 的 DIN） |
| CS0# | PIN 27 | out | 第一片 → CH0/CH1（v3 实际使用） |
| CS1# | PIN 28 | out | 第二片 → CH2/CH3（v3 暂不占用，留作扩展） |
| LDAC# | PIN 29 | out | 两片并联，低有效同步更新输出 |

## 6. ESP32 → FPGA SPI 链路层

### 6.1 电气与时序参数

| 参数 | 值 | 理由 |
| --- | --- | --- |
| SPI Mode | **0**（CPOL=0, CPHA=0） | CS# 空闲低电平，SCLK 空闲低 |
| 位序 | **MSB first** | 与 Arduino `SPI.transfer(uint32_t)` 默认一致；v3 无多字节整数，不存在端序冲突 |
| SCLK | **4 MHz**（默认） | FPGA 过采样需 `f_clk ≥ 4×SCLK`：定稿 `f_clk = 27 MHz`（§3.2），27/4 = 6.75×。上限 = `f_clk/5` ≈ 5.4 MHz；若将来引入 PLL 提高 `f_clk`，可同比例上调 |
| 事务 | 1 帧 = 1 次 CS# 拉低 | 帧边界 = CS# 边界，FPGA 状态机简单且无失步风险 |
| CS#↔SCLK | setup/hold ≥ 100 ns | Arduino `SPISettings` 自带间隔；FPGA 侧做 2 FF 同步 |
| 帧率 | 稳态 40–80 帧/秒；波形重载突发 ≈ 3.7 k 帧/秒（64 帧 / 17.2 ms） | 见 §3.4、§6.2 Type-B |

### 6.2 帧格式

**Type-R：寄存器读写（定长 8 字节）** —— 承接 [example1.md](file:///d:/Source/git/Mini-4-Channel-AWG/documents/example1.md#L83-L97) 的草案并修正其缺陷（原文把 DATA 定义为"根据 CMD 不同填入"的可变语义、CRC 用 XOR）：

```text
byte0  SYNC  = 0xA5
byte1  CMD   = 0x01 写(W_REG32) | 0x02 读(R_REG32) | 0x03 写并回读确认(RW_REG32)
byte2  REG   = 寄存器地址 (见 §7.1)
byte3  DATA[31:24]   (MSB)
byte4  DATA[23:16]
byte5  DATA[15:8]
byte6  DATA[7:0]     (LSB)
byte7  CRC8 = crc8(bytes0..6)     # poly 0x07, init 0x00, 无反转
```

MISO 回读相位：FPGA 在 byte3..byte6 的 4 个 SCLK 周期内移出 32-bit 读数据，byte7 移出 8-bit 状态字。
**注意**：ESP32 发出 byte1/byte2 时 FPGA 尚不知道地址，故读数据从 **byte3 起** 有效——ESP32 侧必须丢弃 MISO 前 2 字节（标准 SPI 从机 2 字节延迟）。

**Type-G：多寄存器合并写（变长，生产路径每槽 1 帧）**
```text
byte0  SYNC = 0xA5
byte1  CMD  = 0x06
byte2  COUNT = n  (1..8)
byte3..  n × 5 字节子帧： { REG, DATA[31:24], DATA[23:16], DATA[15:8], DATA[7:0] }
末字节   CRC8 (覆盖 byte0 起全部字节)
```
帧长 = 3 + 5n + 1 = **4 + 5n** 字节。整帧 CRC 通过后一次性批量提交（§7.2 影子寄存器），因此 `phase_inc` 与 `amp_scale` 原子生效，不存在"新频率 × 旧幅度"中间态（§4 要点 5）。
典型每槽帧：`n=4`，`REG = 0x10 / 0x11 / 0x18 / 0x19` → 24 B = 48 µs @4 MHz。

**Type-W：波形表单点写（定长 8 字节）**
```text
byte0  SYNC = 0xA5   byte1 CMD = 0x05
byte2  FLAGS = { [7:3] rsv=0, [2] autoinc=0, [1:0] ch }
byte3  addr[11:8]  (高 4 bit 填 0)      byte4  addr[7:0]
byte5  data[11:8]  (高 4 bit 填 0)      byte6  data[7:0]
byte7  CRC8
```
（用于调试；生产路径用 Type-B。）

**Type-B：波形表 Burst（变长，别名 `WAVE_BURST`）**
```text
byte0  SYNC = 0xA5
byte1  CMD  = 0x04
byte2  FLAGS = { [7:3] rsv=0, [2] autoinc, [1:0] ch }
byte3  start_addr[11:8]  (高 4 bit 填 0)
byte4  start_addr[7:0]
byte5..  N × 2 字节波形数据, MSB first, 每点 12-bit 有效 (高 4 bit 填 0)
末字节   CRC8 (覆盖 byte0 起全部字节)
```
由 **CS# 上升沿定界**：FPGA 在 CS# 抬升时校验 CRC 并对 `addr` 自增区做提交。
帧长 = 5 + 2N + 1 = 6 + 2N 字节；`N` 建议 64 → 一帧 134 B。
整表 4096 点 = 64 帧 = 8576 B ≈ **17.2 ms** @4 MHz（与 §3.4、§11 Q5 一致）。

**CMD 取值汇总**

| CMD | 帧型 | 帧长 | 用途 |
| --- | --- | --- | --- |
| `0x01` | Type-R 写 | 8 B | 写单个 32-bit 寄存器（调试/低频配置） |
| `0x02` | Type-R 读 | 8 B | 读单个 32-bit 寄存器 |
| `0x03` | Type-R 写读 | 8 B | 写并同帧回读确认 |
| `0x04` | Type-B | 6+2N B | 波形表 Burst（`WAVE_BURST`） |
| `0x05` | Type-W | 8 B | 波形表单点写（调试） |
| `0x06` | Type-G | 4+5n B | 多寄存器原子合并写（**每 25 ms 槽的生产路径**） |

### 6.3 校验与错误响应

- 每帧必须通过 `SYNC + CMD + 帧长 + CRC8` 四重判定：`SYNC/CMD` 逐字节匹配，帧长由 **CS# 上升沿**推得并与 `CMD` 的定长约定比对（Type-R/W 须为 8 B，Type-G 须为 `4+5n` B，Type-B 须为 `6+2N` B），任一失败则**整帧丢弃、寄存器保持**。
- FPGA 不重传、不应答错误码帧；错误只累计进 `STATUS`（`CRC_ERR`/`FRAME_ERR`），由 ESP32 轮询或 `IRQ#` 感知。
- 半截帧（CS# 提前抬升）→ `FRAME_ERR++`，状态机立即回到 IDLE，不允许残留移位数据污染下一帧。
- 地址越界（`REG` 未实现）→ 写忽略、`FRAME_ERR++`；读返回 `0xDEADBEEF`。

## 7. FPGA 侧接口契约

### 7.1 寄存器映射
均输出（`EN_ALL=0` 且看门狗使能）
`REG` 为 8-bit，仅下表地址合法。默认复位值全部为"静默"态。

| REG | 名称 | R/W | 复位 | 位域 |
| --- | --- | --- | --- | --- |
| `0x00` | `ID` | RO | — | `0x4D394E31` ("M9N1")，链路连通性握手 |
| `0x01` | `CTRL` | RW | `0x80` | `[0]`EN_ALL `[1]`SW_RESET(自清) `[2]`WAVE_ARM `[3]`USE_BAL `[7]`WDG_EN。复位值 `0x80` = **看门狗开、输出关**（与 §8 一致） |
| `0x02` | `STATUS` | RO | `0x00` | `[0]`RUNNING `[1]`WAVE_BUSY `[2]`CRC_ERR(粘滞) `[3]`FRAME_ERR(粘滞) `[4]`WDG_TIMEOUT `[5]`CLK_LOST |
| `0x03` | `WDT_PRESET` | RW | `20` | 看门狗超时，单位 = `2^18 × t_clk`（27 MHz 下 9.71 ms/格，默认 20 格 ≈ 194 ms） |
| `0x10`–`0x13` | `PHASE_INC0..3` | RW | 0 | 32-bit 相位步进 |
| `0x14`–`0x17` | `PHASE_OFF0..3` | RW | 0 | 32-bit 相位偏移 |
| `0x18`–`0x1B` | `AMP_SCALE0..3` | RW | 0 | `[11:0]` 幅度增益 |
| `0x1C`–`0x1F` | `FREQ_WIRE0..3` | RW | 0 | `[7:0]` 线上频率字节，仅诊断/回读，不参与逻辑 |
| `0x20` | `SLOT_INFO` | RW | 0 | `[1:0]`slot 号 `[7:4]`seq 回显，用于对齐诊断 |
| `0x30` | `WAVE_ADDR` | RW | 0 | Burst 自动增指针 |
| `0x31` | `WAVE_KEY` | WO | 0 | 写 `0x5A` 才允许 `WAVE_ARM` 生效，防误改表 |

`EN_ALL=0` 或 `WDG_TIMEOUT=1` 时，硬件强制四通道 `amp_scale←0`，**不受寄存器值影响**（见 §8）。
本通路只使用 `0x10/0x11`、`0x18/0x19`、`0x1C/0x1D`；CH2/CH3 寄存器存在但不由 BLE 驱动。

### 7.2 需新增的模块与信号级契约

当前 [dds_top.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_top.py) 的 `phase_inc[]`/`amp_scale[]` 是**未连接的裸 `Signal`**，`spi_w_*` 同样是裸信号，且 `elaborate()` 内没有任何 `platform.request()`/`io.Buffer`——工程尚不是可落地顶层。本段要求新增：

```python
class SpiSlave(Elaboratable):
    # 输入(引脚侧): sclk, cs_n, mosi      输出: miso, irq_n(开漏)
    # 输出(内部):
    reg_w_stb   = Signal()           # 1 拍脉冲：提交一个寄存器写（Type-G 为连续 n 拍）
    reg_apply   = Signal()           # 1 拍脉冲：把待生效影子组原子搬入工作寄存器
    reg_addr    = Signal(8)
    reg_wdata   = Signal(32)
    reg_rdata   = Signal(32)         # 由 RegFile 组合驱动，按字节移出
    status      = Signal(8)
    wave_stb    = Signal()           # Burst 提交完成
    wave_addr   = Signal(12)
    wave_data   = Signal(16)         # 高 12 bit 有效
    wave_en     = Signal()
    err_crc     = Signal()           # 脉冲
    err_frame   = Signal()           # 脉冲
```

`SpiSlave` → `RegFile` → `DdsTop.{phase_inc[], phase_offset[], amp_scale[]}` 与 `DdsTop.{spi_w_en, spi_w_ch_sel, spi_w_addr, spi_w_data}`（后者现有写路由逻辑可直接复用，见 [dds_top.py#L85-L92](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_top.py#L85-L92)）。

内部实现要求：
1. `sclk`/`cs_n`/`mosi` 先经 **2 FF 同步器**进入 `clk27` 域。
2. SCLK 用 `f_clk` **过采样**（中心点采样：连续 2 拍同值视为稳定），不使用 `sclk` 作时钟域——避免 GCLK 布线约束与多域 CDC。
3. 帧状态机 `IDLE → BYTE（内部移位 8 × BIT，按 SCLK 过采样）→ CS# 上升沿 → CRC → COMMIT/ABORT`，全部在 `clk27` 域；`BYTE` 数不定，由 CS# 定界后按 §6.3 与 `CMD` 的定长约定比对。
4. CRC-8（poly `0x07`）用 8 级 XOR 树组合实现，每字节一次。
5. `RegFile`：波形参数寄存器（`PHASE_INC*`/`PHASE_OFF*`/`AMP_SCALE*`/`FREQ_WIRE*`/`SLOT_INFO`）写入**影子组**，整帧 CRC 通过后由 `reg_apply` **一拍**搬入工作寄存器——因此 Type-G 的 `n` 个子帧对外表现为同时生效，不存在"新频率 × 旧幅度"中间态（§4 要点 5）。`CTRL`/`WDT_PRESET`/`WAVE_KEY` 可直写。
6. `irq_n` = `open_drain(WAVE_BUSY | err_latched)`，仅用于 Burst 背压与错误告警；稳态下 ESP32 靠 `R_REG32` 轮询 `STATUS`，`IRQ#` 为可选加速路径。

## 8. 安全与失效行为（电刺激输出，不可省略）

| 事件 | 检测方 | 动作 | 时限 |
| --- | --- | --- | --- |
| BLE 断开 (`onDisconnect`) | ESP32 | 立即 SPI 写 `CTRL=0` + 两通道 `AMP=0` | ≤ 10 ms |
| 300 ms 未收到有效 B0 | ESP32 | 排程器进 IDLE，写静默帧 | ≤ 25 ms（下一槽） |
| B0 该通道取值越界 | ESP32 | 该通道本 100 ms 静默 | 即时 |
| SPI 帧 CRC 连续 2 次失败 | ESP32 | 写静默帧 + 上报 APP（`B1` 或自定义诊断） | ≤ 50 ms |
| **FPGA 看门狗超时**（`WDT_PRESET` 内无有效写帧） | FPGA | **硬件强制 `amp_scale=0`，置 `WDG_TIMEOUT`**，须由 ESP32 写 `CTRL` 才解除 | 默认 ≈194 ms |
| FPGA 上电/复位 | FPGA | 所有寄存器 0 → 输出静默（波形表除外：上电即为预置的单极脉冲，见 §9 I7，但因 `EN_ALL=0`/`amp_scale=0`/`phase_inc=0` 仍无输出） | — |

关键原则：**FPGA 侧的静默必须可由硬件独立达成**，不依赖 ESP32 正确性。ESP32 崩溃/烧录/固件跑飞时，看门狗是最后一道防线，因此 `WDG_EN` 复位默认**开启**。
`soft_limit` 的双重执行：ESP32 在 §3.3 合成时裁剪一次；FPGA 侧**不**重复裁剪（v3 未定义 FPGA 侧上限），故 `soft` 一旦丢失会退化为 200 满量程。若需硬件级强约束，可在 `0x04/0x05` 增设 `AMP_LIMIT0/1`（列为未决 §11 Q4）。

## 9. 与现有代码的不一致清单（已逐项处置）

下表 7 项已全部处置完毕。原"现状证据"行号是处置前的状态，现已失效，可在 git 历史中追溯。

| ID | 冲突 | 处置结果 | 落点 |
| --- | --- | --- | --- |
| **I1** | `temp_pins.py` 不是可运行代码（无 `import`，`Resource(...)` 写成 class body 悬空表达式） | 新建 `AwgPlatform(TangNano9kPlatform)`，用 `add_resources()` 注册 `esp32_spi` / `dac_spi`，两个资源的 `request()` 均已实测返回正确子信号；`temp_pins.py` 已删除（全仓库无引用）。文件**刻意不叫 `platform.py`**：`build_and_program.py` 以 `python src/awg/<design>.py` 运行会使 `sys.path[0]` 指向本目录，届时 `amaranth.tracer` 的 `import platform` 会被本地同名模块遮蔽 | [awg_platform.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/awg_platform.py) |
| **I2** | DDS 时钟未落实（注释 50 MHz、板载只有 27 MHz、无 PLL、`clk_dds` 悬空） | 定稿 **27 MHz 直用**（§3.2），不引入 PLL；删除 `DdsTop.clk_dds` 悬空信号，顶层改为注释说明 `sync` 域即 `clk27`；新增 `F_CLK_DDS = 27_000_000` 作为频率换算的唯一来源 | [awg_platform.py#L19-L21](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/awg_platform.py#L19-L21)、[dds_top.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_top.py) |
| **I3** | DAC 型号三处矛盾（MCP4728/I2C vs 2×MCP4822/SPI vs DAC8564/16-bit） | 按 README 定稿 **2×MCP4822 / SPI**；`__init__.py` 描述已改写，并注明 16-bit 只是 `data_width` 的预留通路；§5 补 `dac_spi` 引脚表 | [__init__.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/__init__.py) |
| **I4** | `r_en` 算出后未接 `r_port.en`（死逻辑） | 删除 `r_en`，改为显式 `m.d.comb += r_port.en.eq(1)`，并注释说明暂停时 `lut_addr` 不变、无需门控。核查结论：`amaranth.lib.memory` 同步读端口的 `en` 默认 `init=1`，故原代码**功能上并未失效**，风险在于依赖库默认值 | [dds_channel.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_channel.py) |
| **I5** | 幅度缩放为纯增益，不处理直流偏置 | 按决策**保持纯增益**；把"无符号、0 = 零电平、居中正弦需改 `(raw-2^(w-1))*scale+2^(w-1)` 有符号通路"写进 `amp_scale` 声明与乘法段注释 | [dds_channel.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_channel.py) |
| **I6** | 表 4096 点与 v3 每槽 25 ms 周期不同步 | 明确 **`phase_inc` 自由运行**、槽边界不做相位对齐（相位连续），写入相位累加器段注释；每槽相位复位需另加 CTRL 位（未决 §11 Q2） | [dds_channel.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_channel.py) |
| **I7** | `Memory(init=[])` → 波形表全 0，上电无输出 | 新增 `default_wave_table()`：上电预置**单极矩形脉冲**（默认 12.5% 占空比、满量程、基线 0），`DdsChannel(..., lut_init=[...])` 可覆盖；同时注明上电仍需下发 `phase_inc` 才会出波形。**处置中发现的更深问题**：`Memory` 从未登记为 submodule，Amaranth 因此不会把它的端口与内容纳入 IR——即原 `init=[]` 与整张波形表此前根本不参与综合（顺带修正为 `m.submodules.wave_mem = mem = Memory(...)`）。验证：转换后 Verilog 从 111 行增至 4219 行，`wave_mem[0..511] = 12'hfff`、`[512..4095] = 12'h000` | [dds_channel.py](file:///d:/Source/git/Mini-4-Channel-AWG/src/awg/dds_channel.py) |

清单外遗留（实现 SPI 从机时一并处理）：

- `test_dds.py` 仍用 `TangNano9kPlatform(toolchain="Gowin")` 直接综合，与 `build_and_program.py` 的 Apicula 路线不一致，也未改用 `AwgPlatform`；该文件是探索期的手工冒烟脚本，未纳入自动化回归。
- 引脚登记重叠：`esp32_spi` 复用的 PIN 33/34/35/40/41 在 amaranth-boards 的 `tang_nano_9k` 表里已属 `lcd` 资源。只要设计不 `request("lcd")` 就不会生成重复约束；若将来要同时用屏幕与本 SPI，必须重新分针。此点已写入 `AwgPlatform` docstring。

## 10. 验证计划

按层递进，每层可独立判定通过：

1. **纯 Python 单测（`documents` 侧公式）**
   `freq_wire → Hz → phase_inc`、`s/a/soft → amp12`、CRC-8 向量。对拍协议示例：`0xB00000000A0A0A0A000A141E0000000000000065` 必须解出 `seq=0, method=0, freqA=[10,10,10,10], ampA=[0,10,20,30]`，且 B 通道因 `101>100` **整通道静默**。
2. **Amaranth 仿真（`sim()`）**
   用 Python 生成 SCLK/CS#/MOSI 波形，喂给 `SpiSlave`，断言：正确帧 → `reg_w_stb` 单拍 + 正确 `reg_addr/reg_wdata`；**Type-G `n=4` 帧 → 4 拍 `reg_w_stb` + 其后恰好 1 拍 `reg_apply`，且 `reg_apply` 前工作寄存器全程不变**；篡改 CRC → 仅 `err_crc` 脉冲且寄存器不变；CS# 中途抬升 → `err_frame` 且下一帧正常。
3. **回环（无 ESP32）**
   ESP32 跑 `SPI_MASTER_LOOPBACK_TEST`：写 `PHASE_INC0=0x12345678` → `R_REG32` 读回比对，连写 1000 帧统计 `CRC_ERR==0`。
4. **示波器/逻辑分析仪**
   测 SCLK 在 PIN 33 上的实际边沿（确认过采样未误判）、CS# setup/hold、`dac_out[0]` 用逻辑分析仪抓 4 路 12-bit，验证频率与 `phase_inc` 表一致（±0.05%）。
5. **端到端**
   真机 APP 连接 `47L121000` → 下发 B0（A 通道 `freq=10`, `amp={0,10,20,30}`）→ 示波器看 CH0 输出为 10 Hz、幅度按 0/25%/50%/75% 四档每 25 ms 递进；中途拔 BLE → CH0 必须在 ≤200 ms 内归零。
6. **抖动实测**
   连续 10 分钟，逻辑分析仪抓 `dac_out` 更新沿间隔，统计 25 ms 槽的实际偏差分布（判定 §11 Q1）。

## 11. 未决问题

| ID | 问题 | 阻塞级别 |
| --- | --- | --- |
| Q1 | ESP32 逐格推送的 25 ms 节拍抖动是否满足验收？需 §10.6 实测数据。若不满足，改由 FPGA 播放表（推翻 §0 决策 2） | 中（可实测后定） |
| Q2 | 槽位切换时 DDS 相位是否需强制对齐（每槽 `phase_acc←0`）？影响波形连续性与体感 | 低 |
| Q3 | `bal1/bal2` 的实际响应曲线未知（v3 未公布），当前不参与计算。是否需实测标定？ | 低 |
| Q4 | `soft_limit` 是否需要 FPGA 侧硬件冗余执行（`AMP_LIMIT` 寄存器）？涉及安全等级判定 | 中 |
| Q5 | 恒定绝对脉宽是否为需求？若是，启用 `WAVE_BURST` 重载并需重排 25 ms 槽预算（17.2 ms 重载 + 4 帧参数写，余量仅约 7.8 ms） | 中 |
| Q6 | CH2/CH3 的用途：既然只用 v3 的 A/B，剩余两通道是否用于其他信源（如本地按键/上位机），需在顶层预留仲裁 | 低 |
