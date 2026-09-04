"""冒烟测试: LED 跑马灯, 验证 amaranth -> Gowin 工具链完整链路。"""

from pathlib import Path

from amaranth import *
from amaranth.build import Platform
from amaranth.lib import io

__all__ = ["Blinky"]

# apicula gowin_pack 生成的 .fs 只有位流数据行, 缺少 Gowin Programmer 解析所需的
# "//" 注释头 (含器件/UserCode 等元信息), 直接烧录会报 "different id-code"。
# 因此构建后需补上与官方头结构一致的注释块 (字段顺序与数量不可随意删减)。
# 其中 CheckSum/UserCode 应等于位流内置的 UserCode: GW1N-9C 下 gowin_pack 写入
# 0xBE67, 若与注释不一致 Programmer 仅提示 "User code mismatch", 不影响烧录。
_GOWIN_FS_USER_CODE = 0xBE67


class Blinky(Elaboratable):
    """驱动 Tang Nano 9K 板载 6 颗 LED 循环点亮 (跑马灯)。

    板载时钟 27MHz, LED 低有效 (invert=True)。亮点以 ``step_hz`` 的速率
    从 LED0 向 LED5 循环移动, 目测即可确认 bitstream 工作正常。
    """

    def __init__(self, clock_freq=27e6, step_hz=4.0):
        # 设计参数
        self.clock_freq = clock_freq  # Hz
        self.step_hz = step_hz        # 亮点每秒移动的 LED 数
        # IO
        self.leds = Signal(6)

    def elaborate(self, platform: Platform) -> Module:
        m = Module()

        # ---- 分频使能: 每 step_period 个时钟周期产生一次移位使能 ----
        step_period = int(self.clock_freq / self.step_hz)
        timer = Signal(range(step_period), reset=step_period - 1)
        do_shift = Signal()

        m.d.sync += do_shift.eq(0)
        with m.If(timer != 0):
            m.d.sync += timer.eq(timer - 1)
        with m.Else():
            m.d.sync += [
                timer.eq(step_period - 1),
                do_shift.eq(1),
            ]

        # ---- 单点亮点的位置驱动: 每次使能左移一位, 溢出时回到 LED0 ----
        runner = Signal(6, reset=1)  # 初始仅 LED0 亮
        with m.If(do_shift):
            with m.If(runner == (1 << (len(runner) - 1))):
                m.d.sync += runner.eq(1)  # 最高位亮 -> 回绕到最低位
            with m.Else():
                m.d.sync += runner.eq(runner << 1)

        # ---- 板载 LED 输出 ----
        # 平台 "led" 资源引脚低有效 (invert=True); RTL 以逻辑有效电平驱动
        # (1 = 点亮), 物理取反由 io.Buffer 结合资源的 invert 自动完成。
        m.d.comb += self.leds.eq(runner)
        for i in range(len(self.leds)):
            led = io.Buffer("o", platform.request("led", i, dir="-"))
            m.submodules[f"led{i}"] = led
            m.d.comb += led.o.eq(self.leds[i])

        return m


def add_gowin_fs_header(fs_path: str | Path) -> None:
    """为 apicula 生成的裸 .fs 补上 Gowin Programmer 需要的 "//" 注释头。

    就地重写目标文件; 若已带注释头则幂等。参考 Gowin IDE 输出 fs 的头部结构。
    """
    import datetime

    header = [
        "//Copyright (C)2014-2023 Gowin Semiconductor Corporation.",
        "//All rights reserved.",
        "//File Title: Bitstream file",
        "//Tool Version: V1.9.12.03 (64-bit)",
        "//Device: GW1NR-9",
        "//Device Version: C",
        "//Part Number: GW1NR-LV9QN88PC6/I5",
        "//Device-package: GW1NR-9C-QFN88",
        "//BackgroundProgramming: OFF",
        f"//CheckSum: 0x{_GOWIN_FS_USER_CODE:X}",
        f"//UserCode: 0x{_GOWIN_FS_USER_CODE:08X}",
        "//LoadingRate: 2.500MHz",
        "//CRCCheck: ON",
        "//Compress: OFF",
        "//Encryption: OFF",
        "//SecurityBit: OFF",
        "//SecureMode: OFF",
        "//JTAGAsRegularIO: OFF",
        "//GAOCRC: 0101110000100111",
        "//MultiBootSPIAddr: 0x00000000",
        "//Created Time: " + datetime.datetime.now().strftime("%a %b %d %H:%M:%S %Y"),
    ]
    path = Path(fs_path)
    # 只保留位流数据行 (跳过已有注释), 避免重复运行叠加
    body = [l for l in path.read_text(encoding="latin1").splitlines() if not l.startswith("//")]
    path.write_text("\r\n".join(header + body) + "\r\n", encoding="latin1")


if __name__ == "__main__":
    from amaranth_boards.tang_nano_9k import TangNano9kPlatform
    # GowinPlatform 默认工具链即为 Apicula (yosys + nextpnr-himbaechel + gowin_pack);
    # 需将 oss-cad-suite/bin 加入 PATH, 或设置 AMARANTH_ENV_APICULA 指向其环境脚本。
    platform = TangNano9kPlatform()

    # 如需 Gowin 官方工具链 (需 GowinIDE):
    # platform = TangNano9kPlatform(toolchain="Gowin")

    # 冒烟: 仅执行综合/布局布线并生成 .fs, 不烧录 (do_program=False)
    platform.build(Blinky(), name="blinky", do_program=False)
    # apicula 生成的裸位流缺少 Gowin Programmer 需要的注释头, 补上后再烧录
    add_gowin_fs_header(Path("build/blinky.fs"))
