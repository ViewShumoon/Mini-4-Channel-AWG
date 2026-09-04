"""构建 (amaranth + Apicula) 并用 Gowin Programmer 烧录到 SRAM。

用法示例:
    python build_and_program.py                # 构建并烧录 src/awg/blinky.py
    python build_and_program.py --design dds_top
    python build_and_program.py --skip-build   # 仅烧录已生成的 build/<design>.fs
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent

# Gowin IDE 安装目录与 oss-cad-suite 工具链目录 (可用环境变量覆盖)
GOWINIDE = os.environ.get("GOWINIDE", r"D:\Applications\Gowin\Gowin_V1.9.12.03_x64")
OSS_BIN = Path(os.environ.get("OSS_CAD_SUITE_BIN", r"D:\Applications\oss-cad-suite\bin"))

PROGRAMMER = Path(GOWINIDE) / "Programmer" / "bin" / "programmer_cli.exe"

# amaranth 生成的 build_<design>.bat 通过 %YOSYS%/%NEXTPNR_HIMBAECHEL%/%GOWIN_PACK%
# 变量调用工具; 若变量未设置则退化为按 PATH 查找 "yosys" 等名字。PATH 中若混入其它
# 同名工具副本 (如 Gowin/conda/其它 EDA 的 yosys), cmd 会解析到缺 DLL 的目标并以
# 0xC0000135 (STATUS_DLL_NOT_FOUND) 启动失败。故这里统一固定为 oss-cad-suite 的绝对路径。
TOOLCHAIN = {
    "YOSYS": OSS_BIN / "yosys.exe",
    "NEXTPNR_HIMBAECHEL": OSS_BIN / "nextpnr-himbaechel.exe",
    "GOWIN_PACK": OSS_BIN / "gowin_pack.exe",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--design", default="blinky",
                        help="src/awg 下的设计模块名 (默认 blinky)")
    parser.add_argument("--device", default="GW1NR-9C", help="目标器件 (默认 GW1NR-9C)")
    parser.add_argument("--skip-build", action="store_true",
                        help="跳过构建, 仅烧录已生成的 .fs")
    args = parser.parse_args()

    fs = ROOT / "build" / f"{args.design}.fs"

    # 1) 构建: 固定 Apicula 工具链绝对路径后执行 src/awg/<design>.py
    #    (脚本内 platform.build 已生成位流并补上 Gowin Programmer 所需的注释头)
    if not args.skip_build:
        missing = [str(p) for p in TOOLCHAIN.values() if not p.is_file()]
        if missing:
            print("错误: oss-cad-suite 工具缺失:\n  " + "\n  ".join(missing),
                  file=sys.stderr)
            return 1
        env = os.environ.copy()
        env["PATH"] = f"{OSS_BIN};" + env.get("PATH", "")
        env.update({k: str(v) for k, v in TOOLCHAIN.items()})
        cmd = [sys.executable, str(ROOT / "src" / "awg" / f"{args.design}.py")]
        print(">>>", " ".join(cmd))
        subprocess.run(cmd, cwd=ROOT, env=env, check=True)

    if not fs.is_file():
        print(f"错误: 未找到 bitstream: {fs}", file=sys.stderr)
        return 1

    # 2) 烧录 SRAM (Programmer V1.9.12.03 中 --run 2 = SRAM Program)
    if not PROGRAMMER.is_file():
        print(f"错误: 未找到 Gowin Programmer: {PROGRAMMER}", file=sys.stderr)
        return 1
    cmd = [str(PROGRAMMER), "-d", args.device, "--run", "2", "--fsFile", str(fs)]
    print(">>>", " ".join(cmd))
    return subprocess.run(cmd, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
