基于你的需求，这里为你提供一套完整的 **4通道任意波形发生器（AWG）** 系统实现方案。

本方案采用 **ESP32-C3** 作为控制核心（负责 BLE 通信与 JSON 解析），**Tang Nano 9K (Gowin GW1NR-9)** 作为 FPGA 波形合成与时序控制核心（基于 **Amaranth HDL** 开发），外部扩展一块 **4通道 16-Bit SPI DAC**（如 DAC8564 / DAC8554 / AD5664），在 FPGA 内部将 12-Bit 波形数据左移对齐驱动 16-Bit DAC。

---

### 一、 系统整体架构与数据流

```text
  [手机 APP]
      │
      │ (BLE 蓝牙发送 JSON 指令)
      ▼
┌──────────────┐      SPI Master      ┌──────────────────────────────────────────────┐
│   ESP32-C3   │ ───────────────────► │              Tang Nano 9K (FPGA)             │
│ (JSON Parser)│ (CS, SCLK, MOSI, MISO)│                                              │
└──────────────┘                      │  ┌────────────┐   ┌────────────────────────┐ │
                                      │  │ SPI Slave  │──►│ Command / RAM Router   │ │
                                      │  └────────────┘   └───────────┬────────────┘ │
                                      │                               │              │
                                      │        ┌──────────────────────┼────────────┐ │
                                      │        ▼                      ▼            ▼ │
                                      │  ┌───────────┐          ┌───────────┐  ...  │
                                      │  │ DDS CH0   │          │ DDS CH1   │  x4   │
                                      │  │ (RAM/Phase│          │ (RAM/Phase│       │
                                      │  └─────┬─────┘          └─────┬─────┘       │
                                      │        └──────────┬───────────┘             │
                                      │                   ▼                         │
                                      │        ┌──────────────────────┐             │
                                      │        │ Quad SPI DAC Driver  │             │
                                      │        └──────────┬───────────┘             │
                                      └───────────────────┼─────────────────────────┘
                                                          │ SPI (CS, SCLK, DIN, LDAC)
                                                          ▼
                                              ┌───────────────────────┐
                                              │    4-Ch 12-Bit DAC    │
                                              │   (如 DAC8554/8564)   │
                                              └───────────────────────┘

```

---

### 二、 通信协议设计

#### 1. 手机 ➔ ESP32-C3：JSON 数据格式

手机通过 BLE 写入特征值（GATT Write）传递波形配置：

* **配置标准波形（正弦/方波/三角波/锯齿波）：**
```json
{
  "ch": 0,
  "type": "sine",
  "freq": 1000.0,
  "amp": 1.0,
  "phase": 0.0
}

```


* **配置点阵任意波形（波形表更新）：**
```json
{
  "ch": 1,
  "type": "custom",
  "freq": 500.0,
  "amp": 0.8,
  "phase": 90.0,
  "points": [2048, 2080, 2112, 2144, ...] 
}

```


*注：`points` 为 4096 个点（12-bit 取值范围 0~4095）*

#### 2. ESP32-C3 ➔ Tang Nano 9K：硬件 SPI 报文协议

数据打包采用固定帧头 + 命令字格式，提升传输效率：

| 字节偏移 | 字段名称 | 字节长度 | 说明 |
| --- | --- | --- | --- |
| `0x00` | `SYNC` | 1 Byte | 固定帧头 `0xAA` |
| `0x01` | `CMD` | 1 Byte | `0x01`: 设频率步进<br>

<br>`0x02`: 设相位偏移<br>

<br>`0x03`: 设幅值缩放<br>

<br>`0x04`: 写 RAM 波形点 |
| `0x02` | `CH` | 1 Byte | 通道编号 (`0x00` ~ `0x03`) |
| `0x03..0x06` | `DATA` | 4 Bytes | 根据 CMD 不同填入 32-bit Phase Accumulator Step、Phase Offset 或 12-bit 幅度值 |
| `0x07` | `CRC/CS` | 1 Byte | 校验和（前 7 字节异或） |

*当上传 custom 波形时，`CMD = 0x04`，后接 `2 Byte Address + 2 Byte Value` 循环批量写入波形 RAM。*

---

### 三、 ESP32-C3 固件实现要点

ESP32-C3 基于 ESP-IDF 或 Arduino 框架，负责将频率 $f_{out}$ 转换为 32-Bit DDS 相位累加步进值 $M$：

$$M = \frac{f_{out} \times 2^{32}}{f_{clk\_dds}}$$

**核心代码逻辑 (C++)：**

```cpp
#include <ArduinoJson.h>
#include <SPI.h>

#define SYS_CLK 50000000.0UL // FPGA 内 DDS 工作时钟 50MHz

void process_waveform_json(const char* json_str) {
    StaticJsonDocument<1024> doc;
    deserializeJson(doc, json_str);

    uint8_t ch = doc["ch"];
    float freq = doc["freq"];
    float amp = doc["amp"]; // 0.0 - 1.0
    
    // 1. 计算 32-bit 相位步进 M
    uint32_t phase_inc = (uint32_t)((freq * 4294967296.0) / SYS_CLK);
    
    // 2. 发送设置频率指令给 FPGA
    send_fpga_cmd(0x01, ch, phase_inc);
    
    // 3. 计算 12-bit 幅度缩放系数 (0 ~ 4095)
    uint32_t amp_scale = (uint32_t)(amp * 4095.0f);
    send_fpga_cmd(0x03, ch, amp_scale);

    // 4. 若为 custom 类型，循环写入 RAM 数据
    if (doc["type"] == "custom") {
        JsonArray points = doc["points"];
        for (size_t i = 0; i < points.size(); i++) {
            uint16_t val = points[i];
            write_fpga_ram(ch, i, val);
        }
    }
}

```

---

### 四、 Amaranth HDL (FPGA) 核心模块实现

Amaranth HDL 支持优雅的 Python 式硬件建模。系统结构包含以下主要子模块：

1. **`SpiSlave`**：解析 ESP32-C3 发送的数据包，产生写 RAM 及配置寄存器信号。
2. **`DdsChannel`**：包含 32 位相位累加器、4096x12-bit BSRAM（双口内存，端口 A 用于 ESP32 写入波形，端口 B 用于 DDS 连续读取）和乘法器（缩放幅度）。
3. **`DacDriver`**： Quad 12-Bit DAC (如 MCP4728) 驱动模块，以高采样率轮询或并发刷新 4 通道 DAC 数据。

#### 1. DDS 单通道逻辑实现 (`dds_channel.py`)

```python
from amaranth import *
from amaranth.lib.memory import Memory

class DdsChannel(Elaboratable):
    def __init__(self, lut_depth_bits=12, dac_bits=12):
        # 外部控制接口
        self.phase_inc    = Signal(32)   # 32位相位步进
        self.phase_offset = Signal(32)   # 32位相位偏移
        self.amp_scale    = Signal(12)   # 12位幅值控制 (0~4095)
        
        # 波形 RAM 写入接口
        self.ram_w_addr   = Signal(lut_depth_bits)
        self.ram_w_data   = Signal(dac_bits)
        self.ram_w_en     = Signal()
        
        # 输出给 DAC Driver 的 12-bit 数据
        self.dac_out      = Signal(dac_bits)

    def elaborate(self, platform):
        m = Module()
        
        # 32-bit 相位累加器
        phase_acc = Signal(32, reset=0)
        m.d.sync += phase_acc.eq(phase_acc + self.phase_inc)
        
        # 实际查表相位 = 累加器相位 + 偏移
        actual_phase = Signal(32)
        m.d.comb += actual_phase.eq(phase_acc + self.phase_offset)
        
        # 取高 12 位作为 RAM 寻址地址
        lut_addr = Signal(12)
        m.d.comb += lut_addr.eq(actual_phase[20..32])
        
        # 定义 4096 x 12-bit BSRAM 存储波形数据
        mem = Memory(shape=12, depth=4096, init=[])
        r_port = mem.read_port(domain="sync")
        w_port = mem.write_port(domain="sync")
        
        m.submodules.r_port = r_port
        m.submodules.w_port = w_port
        
        # 连线：写端口
        m.d.comb += [
            w_port.addr.eq(self.ram_w_addr),
            w_port.data.eq(self.ram_w_data),
            w_port.en.eq(self.ram_w_en)
        ]
        
        # 连线：读端口
        m.d.comb += r_port.addr.eq(lut_addr)
        
        # 12-bit 原始数据幅值缩放 (0 ~ 4095)
        # raw_data * amp_scale / 4096
        scaled_data = Signal(24)
        m.d.sync += scaled_data.eq(r_port.data * self.amp_scale)
        
        # 输出低 12 位有效幅值
        m.d.comb += self.dac_out.eq(scaled_data[12..24])
        
        return m

```

#### 2. Quad DAC 12-Bit 驱动模块 (`dac_driver.py`)

将 FPGA 内部算出的 12-Bit 波形数据 输出给 DAC：
- MCP4728 4通道，使用 I2C 接口与 FPGA 通信
- MCP4822 2通道，使用 2个SPI 接口与 FPGA 通信


#### 3. Top顶层定义与 `tang_nano_9k.py` 板级绑定 (`top.py`)

```python
from amaranth import *
from amaranth_boards.tang_nano_9k import TangNano9KPlatform
from amaranth.build import Resource, Pins, Attr, PinsN

class AwgTop(Elaboratable):
    def elaborate(self, platform):
        m = Module()
        
        # 实例化 4 个 DDS 通道
        dds_channels = [DdsChannel() for _ in range(4)]
        for i, dds in enumerate(dds_channels):
            m.submodules[f"dds_{i}"] = dds
            
        # 实例化 DAC 驱动
        dac_driver = Dac8554Driver()
        m.submodules.dac_driver = dac_driver
        
        # 获取板载资源或自定义 Pin 资源
        # ESP32 SPI Pin mapping (以 Tang Nano 9K 扩展 IO 为例)
        esp32_spi_cs   = platform.request("gpio", 0) # J2 pin
        esp32_spi_sclk = platform.request("gpio", 1)
        esp32_spi_mosi = platform.request("gpio", 2)
        
        # DAC SPI Pin mapping
        dac_sclk = platform.request("gpio", 3)
        dac_mosi = platform.request("gpio", 4)
        dac_cs   = platform.request("gpio", 5)
        dac_ldac = platform.request("gpio", 6)
        
        # 逻辑连接...
        for i in range(4):
            m.d.comb += dac_driver.ch_data[i].eq(dds_channels[i].dac_out)
            
        m.d.comb += [
            dac_sclk.o.eq(dac_driver.dac_sclk),
            dac_mosi.o.eq(dac_driver.dac_mosi),
            dac_cs.o.eq(dac_driver.dac_cs),
            dac_ldac.o.eq(dac_driver.dac_ldac)
        ]
        
        return m

if __name__ == "__main__":
    platform = TangNano9KPlatform()
    # 编译并生成 Gowin IDE 工程文件夹或 Bitstream
    platform.build(AwgTop(), do_program=False)

```

---

### 五、 Gowin IDE 编译与构建工作流

在开发和部署时，推荐使用 **两阶段编译法**，既能发挥 Amaranth Python 构建的快捷性，又方便在 Gowin IDE 中进行 Timing Analyzer 与 Floorplan 分析：

```text
[Amaranth Python代码]
        │
        ├──► 1. platform.build() 自动调用 Gowin 工具链 (gw_sh) 编译直接生成 .fs 烧录文件
        │
        └──► 2. verilog.convert() 生成标准的 Top.v 源代码
                    │
                    ▼
            [导入 Gowin IDE 手动工程]
                    ├── 综合 (GowinSynthesis)
                    ├── 管脚约束 (.cst)
                    └── 布局布线 (Place & Route) ──► 产生 Bitstream (.fs)

```

#### 方法 1：使用 Amaranth 命令行一步生成并调取工具链

设置 Gowin 工具链环境变量：

```bash
# Windows
set PATH=C:\Gowin\Gowin_V1.9.9\IDE\bin;%PATH%

# Linux
export PATH=/opt/gowin/IDE/bin:$PATH

```

在 Python 中直接运行 `python top.py`，Amaranth 会自动生成 Verilog，然后静默启动 Gowin 的 CLI 脚本 `gw_sh` 自动完成综合与布局布线，在 `build/` 目录下生成 `top.fs` 镜像。

#### 方法 2：导出 Verilog 在 Gowin IDE 中进行详细调优（推荐）

若需要手动修改引脚约束（`.cst`）或调整 Gowin 内部 PLL 配置：

1. 使用 Amaranth 将 Python 逻辑导出为纯 Verilog：
```python
from amaranth.back import verilog
top = AwgTop()
with open("awg_top.v", "w") as f:
    f.write(verilog.convert(top, ports=[]))

```


2. 打开 Gowin IDE，创建新工程，芯片型号选择 **GW1NR-LV9QN88PC6/I5 (Tang Nano 9K 芯片)**。
3. 将导出的 `awg_top.v` 导入工程。
4. 添加引脚约束文件 `pins.cst`，设置 IO 电平标准（建议 3.3V LVCMOS）：
```text
IO_LOC "dac_sclk" 31;
IO_PORT "dac_sclk" IO_TYPE=LVCMOS33;
IO_LOC "dac_mosi" 32;
IO_PORT "dac_mosi" IO_TYPE=LVCMOS33;
...

```


5. 点击 **Run Synthesize** 与 **Run Place & Route**，通过 Gowin Programmer 经由板载 USB-JTAG 将 `.fs` 文件固化到 Flash。

---

### 六、 关键硬件接线与注意事项

1. **FPGA ➔ DAC 信号线部署**：
* DAC SPI SCLK 信号速度建议控制在 **20MHz ~ 40MHz** 之间。
* 为防止数字噪音耦合至模拟波形，Tang Nano 9K 与 DAC 模块之间需严格**共地（GND）**，并在 DAC 的 $V_{DD}$ 电源脚旁路一个 $0.1\,\mu\text{F}$ 贴片电容。


2. **模拟后端（AFE）平滑处理**：
* DAC 直接输出的信号带有阶梯状高频采样噪声。建议在 DAC 输出后增加一级由 4 通道运放（如 TL074 或 OPA4354）构成的 **二阶 Sallen-Key 低通滤波器**，截止频率设在 **15kHz ~ 20kHz** 左右，从而输出纯净光滑的模拟正弦波/任意波形。