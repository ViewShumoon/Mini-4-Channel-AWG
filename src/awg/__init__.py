"""Mini 4-Channel AWG - Amaranth HDL 工程包。

目标平台: Tang Nano 9K (Gowin GW1NR-9C)
控制 MCU: ESP32-C3 (BLE -> JSON -> SPI)
DAC: 2x MCP4822 (每片双通道, 12-bit), SPI 通信 —— 与 README.md 一致,
     引脚资源见 awg_platform.AwgPlatform 的 dac_spi (清单 I3)。
     16-bit 数据通路仅为 DdsChannel(data_width=16) 的预留模式, 需更换 DAC 才可用。
"""
