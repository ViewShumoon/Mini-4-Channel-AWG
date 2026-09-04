/*
 * coyote_proto.cpp - 见 coyote_proto.h
 *
 * 本文件不触碰 BLE / SPI 硬件，是纯函数，便于在 PC 上单测对拍。
 */
#include "coyote_proto.h"

/* B0 帧内字节偏移（§2.2） */
#define B0_HEAD      0
#define B0_SEQ_MTHD  1
#define B0_STR_A     2
#define B0_STR_B     3
#define B0_FREQ_A    4   /* 4..7   */
#define B0_AMP_A     8   /* 8..11  */
#define B0_FREQ_B   12   /* 12..15 */
#define B0_AMP_B    16   /* 16..19 */

#define FREQ_MIN 10
#define FREQ_MAX 240
#define AMP_MAX  100
#define STRENGTH_MAX 200

#if AWG_FREQ_DIRECT
#define FREQ_CEIL_HZ 240
#else
#define FREQ_CEIL_HZ 1000
#endif

void coyoteInit(CoyoteState *st, const uint8_t soft[2], const uint8_t bal1[2],
                const uint8_t bal2[2]) {
  memset(st, 0, sizeof(*st));
  for (uint8_t c = 0; c < 2; c++) {
    st->soft[c] = (soft[c] <= STRENGTH_MAX) ? soft[c] : 200;
    st->bal1[c] = bal1[c];
    st->bal2[c] = bal2[c];
    /* 强度从 0 起步：断电不保留，避免上电即以旧强度输出 */
    st->strength[c] = 0;
  }
  st->pendingSeq = 0;
}

/* ----------------------------------------------------------------
 * §3.1 线上字节 → Hz
 * ---------------------------------------------------------------- */
uint16_t awgWireToHz(uint8_t w) {
#if AWG_FREQ_DIRECT
  return (uint16_t)w;                    /* 直接把线上字节当 Hz */
#else
  if (w <= 100) return (uint16_t)w;                       /* 10..100  */
  if (w <= 200) return (uint16_t)((w - 100) * 5 + 100);   /* 101..200 */
  return (uint16_t)((w - 200) * 10 + 600);                /* 201..240 */
#endif
}

/* ----------------------------------------------------------------
 * §3.2 phase_inc = round(f × 2^32 / F_CLK_DDS)，纯 64-bit 整数
 * ---------------------------------------------------------------- */
uint32_t awgPhaseIncFromWire(uint8_t w) {
  uint64_t f = (uint64_t)awgWireToHz(w);
  return (uint32_t)((f * 4294967296ULL + (F_CLK_DDS / 2)) / F_CLK_DDS);
}

/* ----------------------------------------------------------------
 * §3.3 强度合成
 *   s_eff = min(s, soft); gain = (s_eff/200)×(a/100); amp12 = round(gain×4095)
 * 纯整数式：amp12 = (s_eff × a × 4095 + 10000) / 20000
 * ---------------------------------------------------------------- */
uint16_t awgAmp12(uint8_t strength, uint8_t ampWire, uint8_t soft,
                  uint8_t bal1, uint32_t freqHz) {
  uint8_t s_eff = (strength < soft) ? strength : soft;
  uint32_t amp =
      (uint32_t)(((uint32_t)s_eff * (uint32_t)ampWire * 4095u + 10000u) / 20000u);

#if CFG_USE_BAL
  /* amp_eff = amp × (1 + k1×(bal1-128)/128 × (1 - f/f_max))，k1 为千分比常量 */
  int32_t dev = (int32_t)bal1 - 128;
  int32_t freqTermNum = (int32_t)(FREQ_CEIL_HZ - (freqHz > FREQ_CEIL_HZ ? FREQ_CEIL_HZ : freqHz));
  int32_t permille = 1000 + (AWG_BAL_K1_PERMILLE * dev * freqTermNum) /
                                 ((int32_t)128 * FREQ_CEIL_HZ);
  if (permille < 0) permille = 0;
  amp = (uint32_t)(((int64_t)amp * permille) / 1000);
#else
  (void)bal1;
  (void)freqHz;
#endif

  if (amp > 4095u) amp = 4095u;
  return (uint16_t)amp;
}

/* ----------------------------------------------------------------
 * §2.3 method 状态机（单通道）
 * ---------------------------------------------------------------- */
static uint8_t applyMethod(uint8_t s, uint8_t method, uint8_t value,
                           uint8_t soft) {
  /* 设定值超出 0..200 按 0 处理（协议例 3/4） */
  uint8_t v = (value > STRENGTH_MAX) ? 0 : value;
  switch (method & 0x03) {
    case 0x00: return s;                                   /* 不改变 */
    case 0x01: { uint16_t up = (uint16_t)s + v; return (up > soft) ? soft : (uint8_t)up; }
    case 0x02: return (v >= s) ? 0 : (uint8_t)(s - v);     /* max(s-v, 0) */
    default:   return (v > soft) ? soft : v;  /* 0b11 绝对设定: clamp(v,0,soft) */
  }
}

/* ----------------------------------------------------------------
 * §2.5 整通道取值域检查：任一 freq/amp 越界 → 该通道全部 4 组作废
 * ---------------------------------------------------------------- */
static bool channelRangeOk(const uint8_t *freq4, const uint8_t *amp4) {
  for (uint8_t i = 0; i < AWG_SLOT_COUNT; i++) {
    if (freq4[i] < FREQ_MIN || freq4[i] > FREQ_MAX) return false;
    if (amp4[i] > AMP_MAX) return false;
  }
  return true;
}

/* 装载一个通道：无效则整通道静默，有效则逐槽算映射 */
static void loadChannel(AwgSlot *slots, const uint8_t *freq4,
                        const uint8_t *amp4, uint8_t strength, uint8_t soft,
                        uint8_t bal1, bool valid) {
  for (uint8_t i = 0; i < AWG_SLOT_COUNT; i++) {
    if (!valid) {
      slots[i].phaseInc = 0;
      slots[i].amp12 = 0;
      slots[i].freqWire = 0;
      slots[i].enable = 0;
      continue;
    }
    uint16_t hz = awgWireToHz(freq4[i]);
    slots[i].phaseInc = awgPhaseIncFromWire(freq4[i]);
    slots[i].amp12 = awgAmp12(strength, amp4[i], soft, bal1, hz);
    slots[i].freqWire = freq4[i];
    slots[i].enable = (slots[i].amp12 != 0) ? 1 : 0;
  }
}

uint8_t coyoteDecodeB0(CoyoteState *st, const uint8_t *d, uint8_t len,
                       AwgSlot sched[2][AWG_SLOT_COUNT]) {
  if (len < BLE_B0_LEN || d[B0_HEAD] != 0xB0) return COYOTE_B0_BAD_LEN;

  const uint8_t seq = (uint8_t)((d[B0_SEQ_MTHD] >> 4) & 0x0F);
  const uint8_t mthd = d[B0_SEQ_MTHD] & 0x0F;
  const uint8_t mA = (uint8_t)((mthd >> 2) & 0x03);   /* 高 2 bit = A */
  const uint8_t mB = (uint8_t)(mthd & 0x03);          /* 低 2 bit = B */

  /* 校验先于任何写入（§2.5：校验与提交严格分离） */
  const bool validA = channelRangeOk(d + B0_FREQ_A, d + B0_AMP_A);
  const bool validB = channelRangeOk(d + B0_FREQ_B, d + B0_AMP_B);

  /* 强度状态机：与通道级丢弃相互独立（B1 仍回报新强度） */
  st->strength[0] = applyMethod(st->strength[0], mA, d[B0_STR_A], st->soft[0]);
  st->strength[1] = applyMethod(st->strength[1], mB, d[B0_STR_B], st->soft[1]);

  loadChannel(sched[0], d + B0_FREQ_A, d + B0_AMP_A, st->strength[0],
              st->soft[0], st->bal1[0], validA);
  loadChannel(sched[1], d + B0_FREQ_B, d + B0_AMP_B, st->strength[1],
              st->soft[1], st->bal1[1], validB);

  uint8_t flags = COYOTE_B0_OK;
  if (validA) flags |= COYOTE_B0_A_VALID;
  if (validB) flags |= COYOTE_B0_B_VALID;
  if (seq != 0) {
    st->pendingSeq = seq;      /* B1 发出后由调用方清零 */
    flags |= COYOTE_B0_NEED_B1;
  }
  return flags;
}

uint8_t coyoteDecodeBF(CoyoteState *st, const uint8_t *d, uint8_t len) {
  if (len < BLE_BF_LEN || d[0] != 0xBF) return 0;
  uint8_t changed = 0;

  /* soft_*: 0..200，越界保留旧值且不报错 */
  if (d[1] <= STRENGTH_MAX && st->soft[0] != d[1]) { st->soft[0] = d[1]; changed |= 0x01; }
  if (d[2] <= STRENGTH_MAX && st->soft[1] != d[2]) { st->soft[1] = d[2]; changed |= 0x02; }
  /* bal1/bal2: 单字节天然落在 0..255 */
  if (st->bal1[0] != d[3]) { st->bal1[0] = d[3]; changed |= 0x04; }
  if (st->bal1[1] != d[4]) { st->bal1[1] = d[4]; changed |= 0x08; }
  if (st->bal2[0] != d[5]) { st->bal2[0] = d[5]; changed |= 0x10; }
  if (st->bal2[1] != d[6]) { st->bal2[1] = d[6]; changed |= 0x20; }

  /* soft 下调后已存的强度必须重新裁剪，否则会以超限值输出 */
  for (uint8_t c = 0; c < 2; c++)
    if (st->strength[c] > st->soft[c]) st->strength[c] = st->soft[c];

  return changed;
}

void coyoteBuildB1(const CoyoteState *st, uint8_t seq, uint8_t out[BLE_B1_LEN]) {
  out[0] = 0xB1;
  out[1] = seq;
  out[2] = st->strength[0];
  out[3] = st->strength[1];
}
