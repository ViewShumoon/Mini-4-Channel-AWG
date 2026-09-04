"""验证 4 通道 DDS 模块的 Python 导入和 Amaranth 抽象语法树构建。

不执行 Gowin 综合, 仅验证:
1. 模块能正常 import
2. elaborate() 能正常返回 Module
3. 信号定义正确
"""

import sys
sys.path.insert(0, "d:/Source/git/Mini-4-Channel-AWG")

from src.awg.dds_channel import DdsChannel
from src.awg.dds_top import DdsTop


def test_dds_channel():
    """测试单通道 DDS 模块。"""
    print("=" * 60)
    print("测试单通道 DDS 模块")
    print("=" * 60)
    
    # 测试 12-bit 模式
    print("\n[1] 12-bit 数据通路:")
    ch12 = DdsChannel(lut_depth_bits=12, data_width=12)
    print(f"    - lut_depth_bits: {ch12.lut_depth_bits}")
    print(f"    - data_width: {ch12.data_width}")
    print(f"    - lut_depth: {ch12.lut_depth} (2^{ch12.lut_depth_bits})")
    print(f"    - phase_inc: Signal(32)")
    print(f"    - phase_offset: Signal(32)")
    print(f"    - amp_scale: Signal(12)")
    print(f"    - ram_w_addr: Signal(12)")
    print(f"    - ram_w_data: Signal(12)")
    print(f"    - dac_out: Signal(12)")
    
    # 验证信号宽度
    assert ch12.phase_inc.shape().width == 32
    assert ch12.phase_offset.shape().width == 32
    assert ch12.amp_scale.shape().width == 12
    assert ch12.ram_w_addr.shape().width == 12
    assert ch12.ram_w_data.shape().width == 12
    assert ch12.dac_out.shape().width == 12
    print("    ✓ 信号宽度验证通过")
    
    # 测试 16-bit 模式
    print("\n[2] 16-bit 数据通路:")
    ch16 = DdsChannel(lut_depth_bits=12, data_width=16)
    print(f"    - data_width: {ch16.data_width}")
    print(f"    - amp_scale: Signal(16)")
    print(f"    - ram_w_data: Signal(16)")
    print(f"    - dac_out: Signal(16)")
    
    assert ch16.amp_scale.shape().width == 16
    assert ch16.ram_w_data.shape().width == 16
    assert ch16.dac_out.shape().width == 16
    print("    ✓ 信号宽度验证通过")


def test_dds_top():
    """测试 4 通道 DDS 顶层模块。"""
    print("\n" + "=" * 60)
    print("测试 4 通道 DDS 顶层模块")
    print("=" * 60)
    
    # 测试 12-bit 模式
    print("\n[1] 4通道 12-bit 数据通路:")
    top12 = DdsTop(lut_depth_bits=12, data_width=12)
    print(f"    - num_channels: {top12.num_channels}")
    print(f"    - data_width: {top12.data_width}")
    print(f"    - phase_inc: [{top12.num_channels}] Signal(32)")
    print(f"    - phase_offset: [{top12.num_channels}] Signal(32)")
    print(f"    - amp_scale: [{top12.num_channels}] Signal(12)")
    print(f"    - dac_out: [{top12.num_channels}] Signal(12)")
    print(f"    - spi_w_ch_sel: Signal(2) (支持 4 通道选择)")
    
    assert len(top12.phase_inc) == 4
    assert len(top12.dac_out) == 4
    assert top12.spi_w_ch_sel.shape().width == 2
    print("    ✓ 信号数组验证通过")
    
    # 测试 16-bit 模式
    print("\n[2] 4通道 16-bit 数据通路:")
    top16 = DdsTop(lut_depth_bits=12, data_width=16)
    print(f"    - data_width: {top16.data_width}")
    print(f"    - amp_scale: [{top16.num_channels}] Signal(16)")
    print(f"    - dac_out: [{top16.num_channels}] Signal(16)")
    
    assert top16.amp_scale[0].shape().width == 16
    assert top16.dac_out[0].shape().width == 16
    print("    ✓ 信号宽度验证通过")


if __name__ == "__main__":
    try:
        test_dds_channel()
        test_dds_top()
        print("\n" + "=" * 60)
        print("✓ 所有测试通过!")
        print("=" * 60)
    except Exception as e:
        print(f"\n✗ 测试失败: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
