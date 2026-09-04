/*
 * fpga_frame.h - ESP32 → FPGA SPI 帧封装 / 发送 / UART 人工校验输出
 *
 * 帧格式见 documents/esp32-spi-fpga-link-v3.md §6.2：
 *   Type-R (读/写/写读 32-bit 寄存器)  8 B
 *   Type-G (多寄存器原子合并写)        4 + 5n B
 *   Type-W (波形表单点写)              8 B
 *   Type-B (波形表 Burst)              6 + 2N B
 *
 * AWG_SPI_ENABLE=0 时帧只进队列、由 loop() 打印十六进制，不驱动硬件。
 */
#ifndef FPGA_FRAME_H
#define FPGA_FRAME_H

#include <Arduino.h>
#include "awg_config.h"

/* 帧触发原因标签：随帧一起打印，便于人工核对"这一帧该不该发" */
enum AwgFrameReason {
  AWG_R_BOOT = 0,      /* 上电初始化 */
  AWG_R_HANDSHAKE,     /* 读 ID 链路握手 */
  AWG_R_CTRL_ON,       /* CTRL 开输出 */
  AWG_R_CTRL_OFF,      /* CTRL 关输出 */
  AWG_R_SLOT,          /* 每 25ms 槽的 Type-G（生产路径） */
  AWG_R_IDLE_SILENCE,  /* 4 槽播完 / B0 超时 → 静默 */
  AWG_R_BLE_DROP,      /* BLE 断开 → 静默 */
  AWG_R_RANGE_DROP,    /* 通道取值越界 → 该通道静默 */
  AWG_R_CONFIG,        /* BF / 参数配置 */
  AWG_R_STATUS_POLL,   /* 轮询 STATUS */
  AWG_R_FAULT,         /* 故障静默 + 锁死 */
  AWG_R_WAVE_WRITE,    /* Type-W 调试 */
  AWG_R_WAVE_BURST,    /* Type-B 波形表重载 */
  AWG_R_REASON_COUNT
};

typedef struct {
  uint16_t len;                       /* data 有效长度 */
  uint8_t  data[AWG_FRAME_MAX];
  uint8_t  reason;                    /* AwgFrameReason */
  uint8_t  slot;                      /* reason==AWG_R_SLOT 时的槽号，其余 0xFF */
  uint8_t  seq;                       /* 关联的 B0 seq，无则 0xFF */
  uint8_t  tx_attempted;              /* 是否真正走了 SPI 硬件 */
  uint8_t  tx_ok;                     /* SPI 发送 + 回读校验结果 */
  uint32_t read_value;                /* Type-R 读回的 32-bit 数据 */
  uint8_t  read_status;               /* Type-R 回读状态字 */
} AwgFrame;

const char *awgReasonName(uint8_t reason);

/* CRC-8 poly 0x07, init 0x00, 无反转 */
uint8_t awgCrc8(const uint8_t *p, uint16_t n);

/* --- 纯构建函数（返回帧长，不含 CRC 则断言） --- */
uint16_t awgBuildWReg32(uint8_t *buf, uint8_t reg, uint32_t value);
uint16_t awgBuildRReg32(uint8_t *buf, uint8_t reg);
uint16_t awgBuildRWReg32(uint8_t *buf, uint8_t reg, uint32_t value);
uint16_t awgBuildGroup(uint8_t *buf, const uint8_t *regs,
                       const uint32_t *vals, uint8_t n);
uint16_t awgBuildWaveWrite(uint8_t *buf, uint8_t ch, uint16_t addr, uint16_t data12);
uint16_t awgBuildWaveBurst(uint8_t *buf, uint8_t ch, bool autoinc,
                           uint16_t start_addr, const uint16_t *pts, uint16_t n);

/* --- 发送（AWG_SPI_ENABLE=0 时仅入日志队列） --- */
/* 单寄存器写 */
bool awgTxWriteReg(uint8_t reg, uint32_t value, uint8_t reason, uint8_t seq);
/* 单寄存器读，结果写入 outValue（无硬件时为 0） */
bool awgTxReadReg(uint8_t reg, uint32_t *outValue, uint8_t reason, uint8_t seq);
/* 多寄存器原子合并写（每槽生产路径 reason=AWG_R_SLOT） */
bool awgTxGroup(const uint8_t *regs, const uint32_t *vals, uint8_t n,
                uint8_t reason, uint8_t slot, uint8_t seq);
/* CTRL 便捷写 */
bool awgTxCtrl(uint8_t ctrl, uint8_t reason, uint8_t seq);

/* 初始化：日志队列 +（可选）SPI 硬件 */
void awgFrameInit(void);
/* loop 中消费日志队列 */
void awgFramePump(void);
/* SPI 链路故障锁存状态（连续 2 次 CRC 失败后为 true） */
bool awgFrameFaulted(void);
/* 被丢弃（队列满）的帧计数 */
uint32_t awgFrameDroppedCount(void);

#endif /* FPGA_FRAME_H */
