/*
 * coyote_proto.h - coyote/v3 协议编解码 + v3 → AWG 物理量映射
 *
 * 规范: documents/esp32-spi-fpga-link-v3.md §2 (B0/BF/method/seq/校验)
 *                                    §3 (频率、phase_inc、强度合成)
 * 协议原文: DG-LAB-OPENSOURCE coyote/v3/README_V3.md
 */
#ifndef COYOTE_PROTO_H
#define COYOTE_PROTO_H

#include <Arduino.h>
#include "awg_config.h"

/* 单个 25ms 槽的通道参数（§4 sched[2][4]） */
typedef struct {
  uint32_t phaseInc;   /* → REG_PHASE_INC0/1 */
  uint16_t amp12;      /* → REG_AMP0/1, 0..4095 */
  uint8_t  freqWire;   /* → REG_FREQ_WIRE0/1，线上原始字节，仅诊断 */
  uint8_t  enable;     /* 0 = 本槽该通道静默 */
} AwgSlot;

/* 跨 B0 持久的通道状态 */
typedef struct {
  uint8_t strength[2];   /* 当前实际强度 0..200，A=0 / B=1 */
  uint8_t soft[2];       /* 软上限 0..200（BF，NVS 持久） */
  uint8_t bal1[2];       /* 频率平衡（冲击感）0..255，默认 128 */
  uint8_t bal2[2];       /* 强度平衡（脉宽）  0..255，默认 128 */
  uint8_t pendingSeq;    /* 待回告 B1 的 seq，0 = 无 */
} CoyoteState;

/* B0 解码返回标志 */
#define COYOTE_B0_OK       0x01  /* 帧合法，已执行 */
#define COYOTE_B0_A_VALID  0x02  /* A 通道 4 组数据有效 */
#define COYOTE_B0_B_VALID  0x04  /* B 通道 4 组数据有效 */
#define COYOTE_B0_NEED_B1  0x08  /* seq != 0，必须回告 B1 */
#define COYOTE_B0_BAD_LEN  0x10  /* 长度/帧头非法，整帧忽略 */

void coyoteInit(CoyoteState *st, const uint8_t soft[2], const uint8_t bal1[2],
                const uint8_t bal2[2]);

/*
 * 解析 B0：先做 §2.5 整通道取值域校验（校验先于任何写入），
 * 再按 §2.3 method 更新通道强度，最后算出两通道 4 槽的 sched。
 * sched[channel][slot]，channel 0=A → CH0，1=B → CH1。
 * 返回 COYOTE_B0_* 标志位。
 */
uint8_t coyoteDecodeB0(CoyoteState *st, const uint8_t *d, uint8_t len,
                       AwgSlot sched[2][AWG_SLOT_COUNT]);

/*
 * 解析 BF（§2.4）：写入即生效、无回告；越界字段保留旧值且不报错。
 * 返回发生变化的字段位图（bit0..5 = softA softB bal1A bal1B bal2A bal2B）。
 */
uint8_t coyoteDecodeBF(CoyoteState *st, const uint8_t *d, uint8_t len);

/* 组装 B1 回告：0xB1 + seq + strengthA + strengthB（协议原文确认） */
void coyoteBuildB1(const CoyoteState *st, uint8_t seq, uint8_t out[BLE_B1_LEN]);

/* 映射函数（导出便于自测/对拍） */
uint32_t awgPhaseIncFromWire(uint8_t wireByte);
uint16_t awgAmp12(uint8_t strength, uint8_t ampWire, uint8_t soft,
                  uint8_t bal1, uint32_t freqHz);
uint16_t awgWireToHz(uint8_t wireByte);

#endif /* COYOTE_PROTO_H */
