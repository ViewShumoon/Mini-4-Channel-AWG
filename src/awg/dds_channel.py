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


class DdsChannel(Elaboratable):
    """单通道 DDS 模块。
    
    参数:
        lut_depth_bits: LUT 地址位数 (默认 12 = 4096 点)
        data_width:     内部数据通路宽度 (默认 12, 可设为 16)
    """
    
    def __init__(self, lut_depth_bits=12, data_width=12):
        # ---- 外部控制接口 (来自 SPI Slave / Command Router) ----
        self.phase_inc    = Signal(32)   # 32-bit 相位步进 (M = f_out * 2^32 / f_clk)
        self.phase_offset = Signal(32)   # 32-bit 相位偏移
        self.amp_scale    = Signal(data_width)  # 幅度缩放系数 (0 ~ 2^data_width-1)
        
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
    
    def elaborate(self, platform):
        m = Module()
        
        # ============================================================
        # 1. 32-bit 相位累加器
        # ============================================================
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
        mem = Memory(shape=self.data_width, depth=self.lut_depth, init=[])
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
        # 读使能: phase_inc != 0 时使能 (避免暂停时读出无效数据)
        r_en = Signal()
        m.d.comb += r_en.eq(self.phase_inc != 0)
        
        # ============================================================
        # 5. 幅度缩放 (乘法器)
        # ============================================================
        # raw_data (data_width) * amp_scale (data_width) -> 2*data_width
        # 取高 data_width 位作为输出
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
