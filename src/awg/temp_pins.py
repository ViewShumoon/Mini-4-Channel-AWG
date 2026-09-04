class Temp(Elaboratable):
    # ESP32-C3 <-> FPGA 控制 SPI (FPGA 为 Slave)。
    # 复用板上 RGB LCD 引脚 (不用屏时可用), 电平 3.3V。
    # ESP32 side: SCLK=GPIO6 MOSI=GPIO7 CS=GPIO10 MISO=GPIO5 IRQ=GPIO4。
    # 物理位置见 J5-11..16 (PIN33/34/35/40/41/42)。
    Resource("esp32_spi", 0,
        Subsignal("sclk", Pins("33", dir="i")),
        Subsignal("cs",   Pins("34", dir="i")),
        Subsignal("mosi", Pins("35", dir="i")),
        Subsignal("miso", Pins("40", dir="o")),
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