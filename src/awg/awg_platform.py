"""AWG 定制平台: Tang Nano 9K + ESP32-C3 控制 SPI + 2xMCP4822 DAC SPI。

引脚分配依据见 ``documents/esp32-spi-fpga-link-v3.md`` §5。
本模块取代此前的 ``temp_pins.py`` 草稿 (清单 I1)。

注意: 本文件刻意不叫 ``platform.py``。``build_and_program.py`` 以
``python src/awg/<design>.py`` 方式运行, 会使 ``sys.path[0]`` 指向本目录,
届时 ``amaranth.tracer`` 里的 ``import platform`` 将被同名本地模块遮蔽。
"""

from amaranth.build import Attrs, Pins, Resource, Subsignal

from amaranth_boards.tang_nano_9k import TangNano9kPlatform

__all__ = ["AwgPlatform", "F_CLK_DDS"]

# DDS 工作时钟 (Hz)。
# 平台 default_clk 为板载 27 MHz 晶振 (PIN 52)。本工程不引入 PLL (清单 I2)，
#     phase_inc = round(f_pulse_Hz * 2**32 / F_CLK_DDS)
F_CLK_DDS = 27_000_000


class AwgPlatform(TangNano9kPlatform):
    """在 Tang Nano 9K 上追加 AWG 所需的两组 SPI 资源。

    与官方资源表的引脚重叠 (需知):
        PIN 33/34/35/40/41 在 amaranth-boards 的 ``tang_nano_9k`` 定义中已被
        ``lcd`` 资源占用 (de=33, vs=34, clk=35, hs=40, b=[54 53 51 42 41])。
        本工程不使用屏幕，只要设计里不 ``request("lcd")`` 就不会生成重复约束。
        若将来同时需要屏幕与本 SPI，必须重新分配引脚。
    """

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.add_resources([
            # ESP32-C3 <-> FPGA 控制 SPI (FPGA 为 Slave)。
            # 复用板上 RGB LCD 引脚 (不用屏时可用), 电平 3.3V。
            # ESP32 side: SCLK=GPIO6 MOSI=GPIO7 CS=GPIO10 MISO=GPIO5 IRQ=GPIO4。
            # 物理位置见 J5-11..16 (PIN33/34/35/40/41/42)。
            Resource("esp32_spi", 0,
                Subsignal("sclk", Pins("33", dir="i")),
                Subsignal("cs",   Pins("34", dir="i")),
                Subsignal("mosi", Pins("35", dir="i")),
                Subsignal("miso", Pins("40", dir="o")),
                # irq 为 FPGA->ESP32 的开漏输出 (低有效), 需外部 10k 上拉;
                # Gowin 的 PULL_MODE 仅对输入有效, 故不在约束里写。
                Subsignal("irq",  Pins("41", dir="o")),
                Attrs(IO_TYPE="LVCMOS33")),

            # FPGA -> 2x MCP4822 SPI (共享 SCLK/SDI/LDAC, 每片独立 CS)。
            # 纯空闲 IO, 电平 3.3V, 物理位置见 J5-5..9 (PIN25/26/27/28/29)。
            Resource("dac_spi", 0,
                Subsignal("sclk", Pins("25", dir="o")),
                Subsignal("sdi",  Pins("26", dir="o")),
                Subsignal("cs0",  Pins("27", dir="o")),
                Subsignal("cs1",  Pins("28", dir="o")),
                Subsignal("ldac", Pins("29", dir="o")),
                Attrs(IO_TYPE="LVCMOS33")),
        ])
