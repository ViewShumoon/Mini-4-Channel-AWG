"""单通道 DDS (Direct Digital Synthesis) 模块。

包含:
- 32-bit 相位累加器 (0.01Hz 频率分辨率)
- 4096 x 12-bit BSRAM (双口内存)
  - 端口 A: ESP32-C3 写入波形数据
  - 端口 B: DDS 连续读取
- 12-bit x 12-bit 乘法器 (幅度缩放)
- 支持 12/16-bit 数据通路切换
"""

from amaranth import *
from amaranth.lib.memory import Memory


def default_wave_table(depth, width, duty_num=1, duty_den=8):
    """生成上电默认波形表: 单极矩形脉冲 (清单 I7)。

    ``Memory(init=[])`` 会让波形表全 0, 上电后即使参数正确也看不到任何波形。
    这里预置一个"表头高电平、其余为 0"的脉冲, 使 DDS 一跑起来就有可观测输出:
    每个相位周期输出一枚占空比 ``duty_num/duty_den`` (默认 12.5%) 的正脉冲,
    基线为 0, 与 coyote/v3 的单极脉冲语义一致。

    ESP32 侧随后用 Type-B (WAVE_BURST) 或 Type-W (单点) 帧覆盖即可, 默认表只是
    烧录后、下发波形前的兜底。注意默认表不改变 ``phase_inc``, 上电时
    ``phase_inc == 0`` 累加器仍然静止, 必须下发频率才会出波形。
    """
    high = (1 << width) - 1
    n_high = (depth * duty_num) // duty_den
    return [high] * n_high + [0] * (depth - n_high)


class DdsChannel(Elaboratable):
    """单通道 DDS 模块。
    
    参数:
        lut_depth_bits: LUT 地址位数 (默认 12 = 4096 点)
        data_width:     内部数据通路宽度 (默认 12, 可设为 16)
        lut_init:       波形表初始内容, 长度为 2^lut_depth_bits 的序列;
                        为 None 时使用 default_wave_table() 的单极脉冲 (清单 I7)
    """
    
    def __init__(self, lut_depth_bits=12, data_width=12, lut_init=None):
        # ---- 外部控制接口 (来自 SPI Slave / Command Router) ----
        # 32-bit 相位步进 (M = f_out * 2^32 / f_clk), f_clk = 27 MHz 即平台 clk27
        # (清单 I2); 频率分辨率 = 27e6 / 2^32 ≈ 6.3 mHz
        self.phase_inc    = Signal(32)
        self.phase_offset = Signal(32)   # 32-bit 相位偏移
        # 幅度缩放系数 (0 ~ 2^data_width-1): 无符号纯增益, 不含直流偏置 (清单 I5)
        self.amp_scale    = Signal(data_width)
        
        # ---- 波形 RAM 写入接口 (端口 A, 来自 ESP32-C3 SPI) ----
        self.ram_w_addr   = Signal(lut_depth_bits)  # 写入地址
        self.ram_w_data   = Signal(data_width)      # 写入数据
        self.ram_w_en     = Signal()                # 写使能
        
        # ---- DAC 输出接口 ----
        self.dac_out      = Signal(data_width)      # 缩放后的波形数据输出
        
        # ---- 内部状态指示 ----
        self.locked       = Signal()                # DDS 锁定指示 (phase_inc != 0)
        
        # ---- 设计参数 ----
        self.lut_depth_bits = lut_depth_bits
        self.data_width = data_width
        self.lut_depth = 1 << lut_depth_bits
        self.lut_init = lut_init
    
    def elaborate(self, platform):
        m = Module()
        
        # ============================================================
        # 1. 32-bit 相位累加器
        # ============================================================
        # 相位自由运行 (清单 I6): 累加器只由 phase_inc 驱动, 不与 ESP32 的 25 ms
        # 槽边界对齐。相邻槽之间相位连续, 逐格推送不会产生相位跳变; 代价是槽首/
        # 槽尾不保证正好落在波形周期边界上。若需要"每槽相位复位", 应另加一条
        # CTRL 位对 phase_acc 清零 (设计文档 §11 Q2), 而不是改这里的累加逻辑。
        phase_acc = Signal(32, reset=0)
        
        with m.If(self.phase_inc != 0):
            # 正常累加模式
            m.d.sync += phase_acc.eq(phase_acc + self.phase_inc)
            m.d.comb += self.locked.eq(1)
        with m.Else():
            # phase_inc == 0 时暂停 (输出固定相位)
            m.d.comb += self.locked.eq(0)
        
        # ============================================================
        # 2. 实际查表相位 = 累加器相位 + 偏移
        # ============================================================
        actual_phase = Signal(32)
        m.d.comb += actual_phase.eq(phase_acc + self.phase_offset)
        
        # ============================================================
        # 3. 取高 LUT_DEPTH_BITS 位作为 RAM 寻址地址
        # ============================================================
        lut_addr = Signal(self.lut_depth_bits)
        # 相位累加器高位 [32-lut_depth_bits : 31] -> [11:0]
        m.d.comb += lut_addr.eq(actual_phase[32 - self.lut_depth_bits:])
        
        # ============================================================
        # 4. 4096 x 12-bit BSRAM (双口内存)
        # ============================================================
        # 波形表初值: 未显式给定时使用单极脉冲, 避免上电全 0 导致无输出 (清单 I7)
        wave_init = (self.lut_init if self.lut_init is not None
                     else default_wave_table(self.lut_depth, self.data_width))
        # Memory 必须登记为 submodule, 否则它的内容与端口不会进入 IR,
        # init 也就永远不会落到 BSRAM 里。
        m.submodules.wave_mem = mem = Memory(
            shape=self.data_width, depth=self.lut_depth, init=list(wave_init))
        r_port = mem.read_port(domain="sync")  # 端口 B: DDS 读
        w_port = mem.write_port(domain="sync")  # 端口 A: ESP32 写
        
        # ---- 写端口连线 (端口 A) ----
        m.d.comb += [
            w_port.addr.eq(self.ram_w_addr),
            w_port.data.eq(self.ram_w_data),
            w_port.en.eq(self.ram_w_en),
        ]
        
        # ---- 读端口连线 (端口 B) ----
        m.d.comb += r_port.addr.eq(lut_addr)
        # 读端口恒使能 (清单 I4)。同步读端口的 en 虽有默认 init=1, 仍显式驱动以免
        # 依赖库默认值。phase_inc == 0 时 phase_acc 与 lut_addr 都不再变化, 读端口
        # 只是重复读同一地址, 因此无需按 phase_inc 门控 en。
        m.d.comb += r_port.en.eq(1)
        
        # ============================================================
        # 5. 幅度缩放 (乘法器)
        # ============================================================
        # raw_data (data_width) * amp_scale (data_width) -> 2*data_width
        # 取高 data_width 位作为输出
        #
        # 语义约定 (清单 I5): 波形数据与输出均为无符号, 0 = 零电平 (即单极脉冲的
        # 基线), amp_scale 是纯增益 —— 0 静音、满量程 (2^w-1) 近似等值直通、
        # 中间值线性缩小, 不做直流偏置搬移。因此"以中点为零的双极正弦"无法由本
        # 通路直接表达, 其负半周只能由 ESP32 在合成阶段裁剪为 0。若将来要支持
        # 居中波形, 应改为有符号通路 (raw - 2^(w-1)) * scale + 2^(w-1), 而不是
        # 在这里打偏置补丁。
        if self.data_width == 12:
            # 12-bit x 12-bit -> 24-bit, 取高 12 位
            raw_product = Signal(24)
            m.d.sync += raw_product.eq(r_port.data * self.amp_scale)
            m.d.comb += self.dac_out.eq(raw_product[12:])
            
        elif self.data_width == 16:
            # 16-bit x 16-bit -> 32-bit, 取高 16 位
            raw_product = Signal(32)
            m.d.sync += raw_product.eq(r_port.data * self.amp_scale)
            m.d.comb += self.dac_out.eq(raw_product[16:])
        else:
            # 通用处理
            product_width = self.data_width * 2
            raw_product = Signal(product_width)
            m.d.sync += raw_product.eq(r_port.data * self.amp_scale)
            m.d.comb += self.dac_out.eq(raw_product[self.data_width:])
        
        return m
