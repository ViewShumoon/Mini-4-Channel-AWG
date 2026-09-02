"""4 通道 DDS 顶层模块。

实例化 4 个 DdsChannel, 提供统一的管理接口:
- 通道独立控制: 每个通道有独立的 phase_inc / phase_offset / amp_scale
- RAM 写入仲裁: 多个通道的写请求通过路由器 (Router) 统一访问各自通道内部的 BSRAM
- DAC 输出复用: 4 路 12/16-bit 波形数据输出, 可由外部 DAC Driver 采样

数据通路支持 12-bit 和 16-bit 两种模式。
"""

from amaranth import *
from amaranth.lib import memory

from .dds_channel import DdsChannel


class DdsTop(Elaboratable):
    """4 通道 DDS 顶层模块。
    
    参数:
        lut_depth_bits: LUT 地址位数 (默认 12 = 4096 点)
        data_width:     内部数据通路宽度 (默认 12, 可设为 16)
    """
    
    def __init__(self, lut_depth_bits=12, data_width=12):
        self.num_channels = 4
        
        # ---- 全局控制接口 ----
        self.clk_dds      = Signal()      # DDS 工作时钟 (50MHz)
        
        # ---- SPI Slave 写入接口 (来自 ESP32-C3) ----
        # 单写端口多通道共享: 通过 addr 选择目标通道 + 目标通道内的 RAM 地址
        self.spi_w_en     = Signal()      # SPI 写使能
        self.spi_w_ch_sel = Signal(2)     # 通道选择 (0~3)
        self.spi_w_addr   = Signal(lut_depth_bits)  # 目标通道内 RAM 地址
        self.spi_w_data   = Signal(data_width)       # 写入数据
        
        # ---- 通道独立配置接口 (并行访问) ----
        # 每个通道独立的控制信号
        self.phase_inc    = [Signal(32) for _ in range(self.num_channels)]
        self.phase_offset = [Signal(32) for _ in range(self.num_channels)]
        self.amp_scale    = [Signal(data_width) for _ in range(self.num_channels)]
        
        # ---- DAC 输出接口 (4 路 data_width-bit 波形数据) ----
        self.dac_out = [Signal(data_width) for _ in range(self.num_channels)]
        
        # ---- 通道锁定状态指示 ----
        self.locked = [Signal() for _ in range(self.num_channels)]
        
        # ---- 设计参数 ----
        self.lut_depth_bits = lut_depth_bits
        self.data_width = data_width
    
    def elaborate(self, platform):
        m = Module()
        
        # ============================================================
        # 1. 实例化 4 个 DDS Channel
        # ============================================================
        dds_channels = []
        for i in range(self.num_channels):
            ch = DdsChannel(
                lut_depth_bits=self.lut_depth_bits,
                data_width=self.data_width,
            )
            m.submodules[f"dds_{i}"] = ch
            dds_channels.append(ch)
        
        # ============================================================
        # 2. 连接通道独立控制信号
        # ============================================================
        for i in range(self.num_channels):
            m.d.comb += [
                dds_channels[i].phase_inc.eq(self.phase_inc[i]),
                dds_channels[i].phase_offset.eq(self.phase_offset[i]),
                dds_channels[i].amp_scale.eq(self.amp_scale[i]),
                self.dac_out[i].eq(dds_channels[i].dac_out),
                self.locked[i].eq(dds_channels[i].locked),
            ]
        
        # ============================================================
        # 3. RAM 写入路由器 (SPI Slave -> 目标 Channel)
        # ============================================================
        # 当 spi_w_en 有效时, 根据 spi_w_ch_sel 将写入路由到对应通道
        with m.Switch(self.spi_w_ch_sel):
            for i in range(self.num_channels):
                with m.Case(i):
                    m.d.comb += [
                        dds_channels[i].ram_w_addr.eq(self.spi_w_addr),
                        dds_channels[i].ram_w_data.eq(self.spi_w_data),
                        dds_channels[i].ram_w_en.eq(self.spi_w_en),
                    ]
        
        return m
