/*
 * awg_config.h - coyote/v3 → 4 通道 AWG 编译期配置
 *
 * 唯一权威规范: documents/esp32-spi-fpga-link-v3.md
 *   §2 BLE 外设侧   §3 物理量映射   §4 25ms 排程器
 *   §5 引脚与电气   §6 SPI 链路层   §7.1 寄存器契约   §8 安全失效
 */
#ifndef AWG_CONFIG_H
#define AWG_CONFIG_H

/* ============================================================
 * 编译开关
 * ============================================================ */

/*
 * 1 = 真正驱动 SPI 硬件（需要连接 FPGA 引脚）
 * 0 = 只生成帧并通过 UART 打印十六进制，供人工校验（本期默认）
 */
#ifndef AWG_SPI_ENABLE
#define AWG_SPI_ENABLE 0
#endif

/*
 * §3.1 频率刻度:
 *   1 = 直接把线上字节当 Hz（10..240，行为对齐真机，默认）
 *   0 = 使用逆映射（10..1000 Hz）
 */
#ifndef AWG_FREQ_DIRECT
#define AWG_FREQ_DIRECT 1
#endif

/*
 * §2.4 bal1/bal2 是否参与幅度修正。v3 未公布曲线形式（未知项 Q3），
 * 默认 0 = 只接收、持久化、诊断回读，不参与计算。
 */
#ifndef CFG_USE_BAL
#define CFG_USE_BAL 0
#endif

/* CFG_USE_BAL=1 时的一阶修正系数（千分比），无标定依据，仅占位 */
#ifndef AWG_BAL_K1_PERMILLE
#define AWG_BAL_K1_PERMILLE 0
#endif

/* UART 日志波特率 */
#ifndef AWG_LOG_BAUD
#define AWG_LOG_BAUD 115200
#endif

/* ============================================================
 * DDS 时钟 —— 必须与 awg_platform.F_CLK_DDS 一致，否则频率整体偏移
 * ============================================================ */
#define F_CLK_DDS 27000000UL

/* ============================================================
 * ESP32-C3 ↔ FPGA 引脚（§5）
 * ============================================================ */
#define PIN_SPI_SCLK 6   /* GPIO 6  → FPGA PIN 33 */
#define PIN_SPI_MOSI 7   /* GPIO 7  → FPGA PIN 35 */
#define PIN_SPI_CS   10  /* GPIO 10 → FPGA PIN 34, 低有效 */
#define PIN_SPI_MISO 5   /* GPIO 5  → FPGA PIN 40 */
#define PIN_FPGA_IRQ 4   /* GPIO 4  → FPGA PIN 41, FPGA→ESP32 开漏，本脚只读 */

/* §6.1 电气参数: Mode 0 / MSB first / 4 MHz */
#define AWG_SPI_HZ 4000000UL

/* ============================================================
 * SPI 帧常量（§6.2）
 * ============================================================ */
#define SPI_SYNC        0xA5
#define SPI_CMD_W_REG32 0x01
#define SPI_CMD_R_REG32 0x02
#define SPI_CMD_RW_REG32 0x03
#define SPI_CMD_WAVE_BURST 0x04  /* Type-B */
#define SPI_CMD_WAVE_WRITE 0x05  /* Type-W */
#define SPI_CMD_REG_GROUP 0x06   /* Type-G */

/* CRC-8: poly 0x07, init 0x00, 无反转（§6.2） */
#define SPI_CRC8_POLY 0x07
#define SPI_CRC8_INIT 0x00

/* Type-B 预留：N=64 点 → 6+2N = 134 B（§6.2） */
#define AWG_FRAME_MAX 138

/* ============================================================
 * FPGA 寄存器映射（§7.1）
 * ============================================================ */
#define REG_ID          0x00  /* RO 0x4D394E31 "M9N1" */
#define REG_CTRL        0x01  /* RW 复位 0x80 */
#define REG_STATUS      0x02  /* RO */
#define REG_WDT_PRESET  0x03  /* RW 复位 20 */
#define REG_PHASE_INC0  0x10  /* RW 本通路 CH0 = A 通道 */
#define REG_PHASE_INC1  0x11  /* RW 本通路 CH1 = B 通道 */
#define REG_PHASE_OFF0  0x14
#define REG_PHASE_OFF1  0x15
#define REG_AMP0        0x18  /* RW [11:0] 本通路 CH0 = A 通道 */
#define REG_AMP1        0x19  /* RW [11:0] 本通路 CH1 = B 通道 */
#define REG_FREQ_WIRE0  0x1C  /* RW [7:0] 仅诊断/回读 */
#define REG_FREQ_WIRE1  0x1D
#define REG_SLOT_INFO   0x20  /* RW [1:0] slot 号 [7:4] seq 回显 */
#define REG_WAVE_ADDR   0x30
#define REG_WAVE_KEY    0x31  /* WO 写 0x5A 才允许 WAVE_ARM */

#define ID_EXPECT       0x4D394E31UL
#define WDT_PRESET_DEFAULT 20UL

/* CTRL 位域 */
#define CTRL_EN_ALL   0x01
#define CTRL_SW_RESET 0x02  /* 自清 */
#define CTRL_WAVE_ARM 0x04
#define CTRL_USE_BAL  0x08
#define CTRL_WDG_EN   0x80

/* STATUS 位域 */
#define ST_RUNNING     0x01
#define ST_WAVE_BUSY   0x02
#define ST_CRC_ERR     0x04  /* 粘滞 */
#define ST_FRAME_ERR   0x08  /* 粘滞 */
#define ST_WDG_TIMEOUT 0x10
#define ST_CLK_LOST    0x20

#define WAVE_KEY_VALUE 0x5A

/* ============================================================
 * 25ms 槽位排程（§4）
 * ============================================================ */
#define AWG_SLOT_COUNT 4
#define AWG_SLOT_US    25000      /* 每槽 25 ms */
#define AWG_B0_TIMEOUT_MS 300     /* §8: 300ms 无有效 B0 → 静默 */

/* ============================================================
 * BLE GATT（§2.1）
 * ============================================================ */
#define BLE_DEVICE_NAME "47L121000"
#define BLE_UUID_SERVICE_HEARTBEAT "180C"  /* 0x180C: 0x150A 写 / 0x150B 通知 */
#define BLE_UUID_SERVICE_DEVICE    "180A"  /* 0x180A: 0x1500 电量 */
#define BLE_UUID_CHAR_WRITE        "150A"  /* WRITE, ≤20 B (B0/BF) */
#define BLE_UUID_CHAR_NOTIFY       "150B"  /* NOTIFY, B1 */
#define BLE_UUID_CHAR_BATTERY      "1500"  /* READ, 1 B */
#define BLE_BATTERY_VALUE          0x64    /* 固定 100% */
#define BLE_B0_LEN                 20
#define BLE_BF_LEN                 7
#define BLE_B1_LEN                 4

#endif /* AWG_CONFIG_H */
