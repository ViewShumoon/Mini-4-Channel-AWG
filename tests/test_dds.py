"""验证 4 通道 DDS 模块能否正常综合。"""

from .dds_top import DdsTop


def test_build():
    """运行 Amaranth 综合, 生成 Verilog 和 bitstream。"""
    from amaranth_boards.tang_nano_9k import TangNano9kPlatform
    
    # 测试 12-bit 数据通路
    print("Building with 12-bit data path...")
    platform = TangNano9kPlatform()
    top_12bit = DdsTop(lut_depth_bits=12, data_width=12)
    platform.build(top_12bit, name="dds_top_12bit", do_program=False)
    print("12-bit build success!")
    
    # 测试 16-bit 数据通路
    print("Building with 16-bit data path...")
    top_16bit = DdsTop(lut_depth_bits=12, data_width=16)
    platform.build(top_16bit, name="dds_top_16bit", do_program=False)
    print("16-bit build success!")


if __name__ == "__main__":
    test_build()
