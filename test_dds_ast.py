"""验证 DDS 模块的 Amaranth AST 能正常构建。

测试 elaborate() 方法是否能正确返回 Module 对象。
"""

from src.awg.dds_channel import DdsChannel
from src.awg.dds_top import DdsTop


def test_ast_build():
    """测试 Amaranth AST 构建。"""
    print("=" * 60)
    print("测试 Amaranth AST 构建")
    print("=" * 60)
    
    # 测试 12-bit 单通道
    print("\n[1] 12-bit 单通道 AST:")
    ch12 = DdsChannel(lut_depth_bits=12, data_width=12)
    
    class DummyPlatform:
        def request(self, name, *args, **kwargs):
            return None
    
    try:
        platform = DummyPlatform()
        m = ch12.elaborate(platform)
        print(f"    ✓ elaborate() 成功返回 Module")
        submodule_names = []
        for attr in dir(m.submodules):
            if not attr.startswith("_"):
                val = getattr(m.submodules, attr)
                if hasattr(val, '__name__') or 'r_port' in attr or 'w_port' in attr:
                    submodule_names.append(attr)
        print(f"    - submodules: {submodule_names}")
    except Exception as e:
        print(f"    ✗ elaborate() 失败: {e}")
        import traceback
        traceback.print_exc()
        raise
    
    # 测试 16-bit 单通道
    print("\n[2] 16-bit 单通道 AST:")
    ch16 = DdsChannel(lut_depth_bits=12, data_width=16)
    try:
        m = ch16.elaborate(platform)
        print(f"    ✓ elaborate() 成功返回 Module")
    except Exception as e:
        print(f"    ✗ elaborate() 失败: {e}")
        raise
    
    # 测试 4 通道顶层 (12-bit)
    print("\n[3] 4通道 12-bit 顶层 AST:")
    top12 = DdsTop(lut_depth_bits=12, data_width=12)
    try:
        m = top12.elaborate(platform)
        print(f"    ✓ elaborate() 成功返回 Module")
        
        # 统计子模块数量
        submodule_count = 0
        for attr_name in dir(m.submodules):
            if not attr_name.startswith("_") and attr_name != " __get__":
                submodule_count += 1
        print(f"    - submodules count: {submodule_count} (预期 8: 4个dds + 4*2个port)")
        
        # 列出关键 submodule
        key_submodules = [attr for attr in dir(m.submodules) if not attr.startswith("_")]
        print(f"    - submodules: {key_submodules[:10]}...")
    except Exception as e:
        print(f"    ✗ elaborate() 失败: {e}")
        import traceback
        traceback.print_exc()
        raise
    
    # 测试 4 通道顶层 (16-bit)
    print("\n[4] 4通道 16-bit 顶层 AST:")
    top16 = DdsTop(lut_depth_bits=12, data_width=16)
    try:
        m = top16.elaborate(platform)
        print(f"    ✓ elaborate() 成功返回 Module")
    except Exception as e:
        print(f"    ✗ elaborate() 失败: {e}")
        import traceback
        traceback.print_exc()
        raise
    
    print("\n" + "=" * 60)
    print("✓ AST 构建测试全部通过!")
    print("=" * 60)


if __name__ == "__main__":
    test_ast_build()
