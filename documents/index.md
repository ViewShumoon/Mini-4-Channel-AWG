# Main

- Tang Nano 9K
- ESP32-C3 作为 MCU, 手机通过蓝牙传递json格式的波形描述, MCU 解析后生成波形, 使用 SPI 与 Tang Nano 9K 通讯 
- 外部 DAC，4 通道，12-Bit, 不需要并行；使用2个2通道的 MCP4822, SPI 通信
- 使用 amaranth hdl 开发, 和 amaranth-boards 里的 tang_nano_9k.py 定义, 调用 Gowin CLI 进行综合与布局布线 或者使用 apicula
- 不需要显示屏, 触控等外设


## Path

- GowinIDE: D:\Applications\Gowin\Gowin_V1.9.12.03_x64