/*
 * fpga_frame.cpp - 见 fpga_frame.h
 */
#include "fpga_frame.h"

#if AWG_SPI_ENABLE
#include <SPI.h>
#endif

#define LOG_QUEUE_DEPTH 24

static QueueHandle_t s_logQueue = NULL;
static SemaphoreHandle_t s_txMux = NULL;
static uint32_t s_dropped = 0;
static bool s_faulted = false;
static uint8_t s_crcFails = 0;

static const char *s_reasonNames[AWG_R_REASON_COUNT] = {
    "BOOT", "HANDSHAKE", "CTRL_ON", "CTRL_OFF", "SLOT", "IDLE_SILENCE",
    "BLE_DROP", "RANGE_DROP", "CONFIG", "STATUS_POLL", "FAULT",
    "WAVE_WRITE", "WAVE_BURST"};

const char *awgReasonName(uint8_t reason) {
  if (reason >= AWG_R_REASON_COUNT) return "UNKNOWN";
  return s_reasonNames[reason];
}

/* ------------------------------------------------------------------
 * CRC-8: poly 0x07, init 0x00, 无反转（与非反转标准 CRC-8/SAE 同结果）
 * ------------------------------------------------------------------ */
uint8_t awgCrc8(const uint8_t *p, uint16_t n) {
  uint8_t crc = SPI_CRC8_INIT;
  while (n--) {
    crc ^= *p++;
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ SPI_CRC8_POLY)
                         : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

/* ------------------------------------------------------------------
 * 帧构建
 * ------------------------------------------------------------------ */
static void put32(uint8_t *b, uint32_t v) {
  b[0] = (uint8_t)(v >> 24);
  b[1] = (uint8_t)(v >> 16);
  b[2] = (uint8_t)(v >> 8);
  b[3] = (uint8_t)v;
}

uint16_t awgBuildWReg32(uint8_t *buf, uint8_t reg, uint32_t value) {
  buf[0] = SPI_SYNC;
  buf[1] = SPI_CMD_W_REG32;
  buf[2] = reg;
  put32(buf + 3, value);
  buf[7] = awgCrc8(buf, 7);
  return 8;
}

uint16_t awgBuildRReg32(uint8_t *buf, uint8_t reg) {
  buf[0] = SPI_SYNC;
  buf[1] = SPI_CMD_R_REG32;
  buf[2] = reg;
  put32(buf + 3, 0);
  buf[7] = awgCrc8(buf, 7);
  return 8;
}

uint16_t awgBuildRWReg32(uint8_t *buf, uint8_t reg, uint32_t value) {
  buf[0] = SPI_SYNC;
  buf[1] = SPI_CMD_RW_REG32;
  buf[2] = reg;
  put32(buf + 3, value);
  buf[7] = awgCrc8(buf, 7);
  return 8;
}

/* Type-G: 4 + 5n 字节（§6.2），n = 1..8 */
uint16_t awgBuildGroup(uint8_t *buf, const uint8_t *regs,
                       const uint32_t *vals, uint8_t n) {
  if (n < 1) n = 1;
  if (n > 8) n = 8;
  buf[0] = SPI_SYNC;
  buf[1] = SPI_CMD_REG_GROUP;
  buf[2] = n;
  uint16_t i = 3;
  for (uint8_t k = 0; k < n; k++) {
    buf[i++] = regs[k];
    put32(buf + i, vals[k]);
    i += 4;
  }
  buf[i] = awgCrc8(buf, i);
  return (uint16_t)(i + 1);
}

/* Type-W: 8 字节，FLAGS = {rsv[7:3], autoinc[2], ch[1:0]} */
uint16_t awgBuildWaveWrite(uint8_t *buf, uint8_t ch, uint16_t addr,
                           uint16_t data12) {
  buf[0] = SPI_SYNC;
  buf[1] = SPI_CMD_WAVE_WRITE;
  buf[2] = (uint8_t)(ch & 0x03);           /* autoinc = 0 */
  buf[3] = (uint8_t)((addr >> 8) & 0x0F);
  buf[4] = (uint8_t)(addr & 0xFF);
  buf[5] = (uint8_t)((data12 >> 8) & 0x0F);
  buf[6] = (uint8_t)(data12 & 0xFF);
  buf[7] = awgCrc8(buf, 7);
  return 8;
}

/* Type-B: 6 + 2N 字节 */
uint16_t awgBuildWaveBurst(uint8_t *buf, uint8_t ch, bool autoinc,
                           uint16_t start_addr, const uint16_t *pts,
                           uint16_t n) {
  uint16_t i = 0;
  buf[i++] = SPI_SYNC;
  buf[i++] = SPI_CMD_WAVE_BURST;
  buf[i++] = (uint8_t)((autoinc ? 0x04 : 0x00) | (ch & 0x03));
  buf[i++] = (uint8_t)((start_addr >> 8) & 0x0F);
  buf[i++] = (uint8_t)(start_addr & 0xFF);
  for (uint16_t k = 0; k < n; k++) {
    buf[i++] = (uint8_t)((pts[k] >> 8) & 0x0F);
    buf[i++] = (uint8_t)(pts[k] & 0xFF);
  }
  buf[i] = awgCrc8(buf, i);
  return (uint16_t)(i + 1);
}

/* ------------------------------------------------------------------
 * 发送
 * ------------------------------------------------------------------ */
#if AWG_SPI_ENABLE
static bool awgSpiTransferOnce(const AwgFrame *tx, uint8_t *miso) {
  if (tx->len > AWG_FRAME_MAX) return false;
  uint8_t scratch[AWG_FRAME_MAX];
  if (!miso) miso = scratch;
  SPI.beginTransaction(SPISettings(AWG_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_SPI_CS, HIGH);
  digitalWrite(PIN_SPI_CS, LOW);              /* 帧边界 = CS# 下降沿 */
  for (uint16_t i = 0; i < tx->len; i++) miso[i] = SPI.transfer(tx->data[i]);
  digitalWrite(PIN_SPI_CS, HIGH);             /* CS# 上升沿：FPGA 校验并提交 */
  SPI.endTransaction();
  return true;
}
#endif

/* 静默/故障类帧在锁死后仍允许下发 */
static bool reasonBypassFault(uint8_t reason) {
  return reason == AWG_R_IDLE_SILENCE || reason == AWG_R_BLE_DROP ||
         reason == AWG_R_RANGE_DROP || reason == AWG_R_FAULT ||
         reason == AWG_R_CTRL_OFF;
}

static void awgEnqueue(const AwgFrame *f) {
  if (!s_logQueue) return;
  if (xQueueSend(s_logQueue, f, 0) != pdTRUE) s_dropped++;
}

/*
 * 帧下发互斥：BLE 回调任务、esp_timer 任务与 loop 任务都可能推帧，
 * 而 SPI 的帧边界就是 CS# 边界，整帧必须串行、不可交错。
 */
static bool awgTransmit(AwgFrame *f) {
  if (s_faulted && !reasonBypassFault(f->reason)) {
    f->tx_ok = 0;
    awgEnqueue(f);          /* 锁存后仍打印，便于人工确认现场 */
    return false;
  }
  if (s_txMux) xSemaphoreTake(s_txMux, portMAX_DELAY);

#if AWG_SPI_ENABLE
  uint8_t miso[AWG_FRAME_MAX];
  f->tx_attempted = 1;
  bool ok = awgSpiTransferOnce(f, miso);
  if (ok && (f->data[1] == SPI_CMD_R_REG32 || f->data[1] == SPI_CMD_RW_REG32)) {
    /* §6.2：读数据从 byte3 起有效，MISO 前 2 字节必须丢弃 */
    f->read_value = ((uint32_t)miso[3] << 24) | ((uint32_t)miso[4] << 16) |
                    ((uint32_t)miso[5] << 8) | (uint32_t)miso[6];
    f->read_status = miso[7];
    if (f->data[2] == REG_ID && f->read_value != ID_EXPECT) ok = false;
    if (f->read_status & ST_CRC_ERR) ok = false;
  }
  if (ok) {
    s_crcFails = 0;
  } else {
    /* §4 要点 6：重试 1 次，仍失败 → 静默并停机等待人工复位 */
    s_crcFails++;
    bool retry = awgSpiTransferOnce(f, miso);
    if (retry && !(f->read_status & ST_CRC_ERR)) {
      s_crcFails = 0;
      f->tx_ok = 1;
    } else {
      f->tx_ok = 0;
      if (s_crcFails >= 2) s_faulted = true;
      awgEnqueue(f);
      if (s_txMux) xSemaphoreGive(s_txMux);
      return false;
    }
  }
#else
  f->tx_attempted = 0;
  f->tx_ok = 1;
#endif
  awgEnqueue(f);
  if (s_txMux) xSemaphoreGive(s_txMux);
  return true;
}

bool awgTxWriteReg(uint8_t reg, uint32_t value, uint8_t reason, uint8_t seq) {
  AwgFrame f;
  memset(&f, 0, sizeof(f));
  f.reason = reason;
  f.slot = 0xFF;
  f.seq = seq;
  f.len = awgBuildWReg32(f.data, reg, value);
  /* 失败/锁存路径的打印已由 awgTransmit 内部入队完成，此处不再重复 */
  return awgTransmit(&f);
}

bool awgTxReadReg(uint8_t reg, uint32_t *outValue, uint8_t reason,
                  uint8_t seq) {
  AwgFrame f;
  memset(&f, 0, sizeof(f));
  f.reason = reason;
  f.slot = 0xFF;
  f.seq = seq;
  f.len = awgBuildRReg32(f.data, reg);
  bool r = awgTransmit(&f);
  if (outValue) *outValue = r ? f.read_value : 0;
  return r;
}

bool awgTxGroup(const uint8_t *regs, const uint32_t *vals, uint8_t n,
                uint8_t reason, uint8_t slot, uint8_t seq) {
  AwgFrame f;
  memset(&f, 0, sizeof(f));
  f.reason = reason;
  f.slot = slot;
  f.seq = seq;
  f.len = awgBuildGroup(f.data, regs, vals, n);
  return awgTransmit(&f);
}

bool awgTxCtrl(uint8_t ctrl, uint8_t reason, uint8_t seq) {
  return awgTxWriteReg(REG_CTRL, ctrl, reason, seq);
}

/* ------------------------------------------------------------------
 * 初始化与 UART 输出
 * ------------------------------------------------------------------ */
void awgFrameInit(void) {
  if (!s_txMux) s_txMux = xSemaphoreCreateMutex();
  s_logQueue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(AwgFrame));
  s_dropped = 0;
  s_faulted = false;
  s_crcFails = 0;
#if AWG_SPI_ENABLE
  pinMode(PIN_SPI_CS, OUTPUT);
  digitalWrite(PIN_SPI_CS, HIGH);
  pinMode(PIN_FPGA_IRQ, INPUT_PULLUP);
  SPI.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS);
#endif
}

bool awgFrameFaulted(void) { return s_faulted; }
uint32_t awgFrameDroppedCount(void) { return s_dropped; }

static char s_line[600];

void awgFramePump(void) {
  AwgFrame f;
  if (!s_logQueue) return;
  while (xQueueReceive(s_logQueue, &f, 0) == pdTRUE) {
    int n = 0;
    n += snprintf(s_line + n, sizeof(s_line) - n, "[%7.3fs] %-11s",
                  (float)(millis() / 1000.0));
    if (f.reason == AWG_R_SLOT) n += snprintf(s_line + n, sizeof(s_line) - n,
                                              " slot=%u", f.slot);
    n += snprintf(s_line + n, sizeof(s_line) - n, " seq=%s len=%3u",
                  (f.seq == 0xFF) ? "-" : std::to_string(f.seq).c_str(),
                  (unsigned)f.len);
#if AWG_SPI_ENABLE
    n += snprintf(s_line + n, sizeof(s_line) - n, " tx=%s",
                  f.tx_ok ? "OK " : "FAIL");
    if (f.read_status || f.read_value)
      n += snprintf(s_line + n, sizeof(s_line) - n, " rdata=%08lX st=%02X",
                    (unsigned long)f.read_value, f.read_status);
#else
    n += snprintf(s_line + n, sizeof(s_line) - n, " [SIM]");
#endif
    n += snprintf(s_line + n, sizeof(s_line) - n, " | ");
    for (uint16_t i = 0; i < f.len && n < (int)(sizeof(s_line) - 4); i++)
      n += snprintf(s_line + n, sizeof(s_line) - n, "%02X ", f.data[i]);
    if (s_faulted) n += snprintf(s_line + n, sizeof(s_line) - n, "| FAULT-LATCHED");
    s_line[n < (int)sizeof(s_line) - 2 ? n : (int)sizeof(s_line) - 2] = 0;
    Serial.println(s_line);
  }
}
