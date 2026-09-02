"""冒烟测试: LED 跑马灯, 验证 amaranth -> Gowin 工具链完整链路。"""

from amaranth import *
from amaranth.build import Platform

__all__ = ["Blinky"]


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

        # 板载 LED 低有效, 输出取反
        m.d.comb += self.leds.eq(~runner)

        return m


if __name__ == "__main__":
    from amaranth_boards.tang_nano_9k import TangNano9kPlatform

    platform = TangNano9kPlatform(toolchain="Gowin")
    # 冒烟: 仅执行综合/布局布线并生成 .fs, 不烧录 (do_program=False)
    platform.build(Blinky(), name="blinky", do_program=False)
