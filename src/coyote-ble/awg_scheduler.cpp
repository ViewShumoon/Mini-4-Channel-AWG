/*
 * awg_scheduler.cpp - 见 awg_scheduler.h
 */
#include "awg_scheduler.h"
#include "fpga_frame.h"
#include <esp_timer.h>

/*
 * 静默态 CTRL 取值说明：
 * §8 表格写"立即 SPI 写 CTRL=0"，指的是清掉 EN_ALL（bit0）；
 * 而 §7.1 复位值 = 0x80（看门狗开、输出关），§8 关键原则要求
 * "WDT_EN 复位默认开启"作为 ESP32 失控时的最后防线。
 * 因此本机静默统一写 CTRL_WDG_EN（关输出、保留看门狗），不写全 0。
 */
#define CTRL_SILENT CTRL_WDG_EN
#define CTRL_ACTIVE (CTRL_WDG_EN | CTRL_EN_ALL)

static AwgSlot s_sched[2][AWG_SLOT_COUNT];
static volatile uint8_t s_slot = AWG_SLOT_COUNT;
static esp_timer_handle_t s_timer = NULL;
static uint32_t s_lastB0Ms = 0;
static uint8_t s_seq = 0xFF;
static bool s_idle = true;
static bool s_outputOn = false;

/* 每槽生产帧：Type-G n=4，phase_inc 与 amp_scale 同帧原子生效（§4 要点 3/5） */
static void pushSlot(uint8_t idx) {
  const uint8_t regs[4] = {REG_PHASE_INC0, REG_PHASE_INC1, REG_AMP0, REG_AMP1};
  const uint32_t vals[4] = {
      s_sched[0][idx].phaseInc, s_sched[1][idx].phaseInc,
      (uint32_t)s_sched[0][idx].amp12, (uint32_t)s_sched[1][idx].amp12};
  awgTxGroup(regs, vals, 4, AWG_R_SLOT, idx, s_seq);
}

/* 静默：先清两通道幅度，再关总输出 */
static void emitSilence(uint8_t reason) {
  const uint8_t regs[2] = {REG_AMP0, REG_AMP1};
  const uint32_t vals[2] = {0, 0};
  awgTxGroup(regs, vals, 2, reason, 0xFF, s_seq);
  awgTxCtrl(CTRL_SILENT, AWG_R_CTRL_OFF, 0xFF);
  s_outputOn = false;
}

static void enterIdle(uint8_t reason) {
  s_slot = AWG_SLOT_COUNT;
  if (!s_idle) {
    s_idle = true;
    emitSilence(reason);
  }
}

/* esp_timer 回调运行于 esp_timer 任务上下文（非 ISR），允许短阻塞 */
static void slotTimerCb(void *arg) {
  (void)arg;
  if (s_slot >= AWG_SLOT_COUNT) return;   /* 期间已被 schedAbort / 新 B0 接管 */
  uint8_t next = (uint8_t)(s_slot + 1);
  if (next >= AWG_SLOT_COUNT) {
    /* §4 要点 2：4 槽播完即静默，不循环重播 */
    s_slot = AWG_SLOT_COUNT;
    s_idle = true;
    emitSilence(AWG_R_IDLE_SILENCE);
    return;
  }
  s_slot = next;
  pushSlot(next);
  esp_timer_start_once(s_timer, AWG_SLOT_US);
}

void schedInit(void) {
  const esp_timer_create_args_t args = {.callback = &slotTimerCb,
                                        .name = "awg_slot"};
  if (s_timer == NULL) esp_timer_create(&args, &s_timer);
  s_idle = true;
  s_slot = AWG_SLOT_COUNT;
}

void schedBoot(void) {
  uint32_t id = 0;
  awgTxReadReg(REG_ID, &id, AWG_R_HANDSHAKE, 0xFF);
  awgTxWriteReg(REG_CTRL, CTRL_SILENT, AWG_R_BOOT, 0xFF);
  awgTxWriteReg(REG_WDT_PRESET, WDT_PRESET_DEFAULT, AWG_R_BOOT, 0xFF);
  const uint8_t regs[4] = {REG_PHASE_INC0, REG_PHASE_INC1, REG_AMP0, REG_AMP1};
  const uint32_t vals[4] = {0, 0, 0, 0};
  awgTxGroup(regs, vals, 4, AWG_R_BOOT, 0xFF, 0xFF);
  s_outputOn = false;
  s_idle = true;
}

void schedLoad(const AwgSlot sched[2][AWG_SLOT_COUNT], uint8_t seq,
               bool anyValid) {
  memcpy(s_sched, sched, sizeof(s_sched));
  s_seq = seq;
  s_lastB0Ms = millis();

  if (!anyValid) {
    /* 两通道都被 §2.5 整通道丢弃 → 本 100ms 全静默，不开输出。
     * 无条件 emitSilence：即便已处于 idle 也重发一次静默帧，
     * 保证"每一被丢弃的 B0 都对应一条可见的静默动作"。 */
    if (s_timer) esp_timer_stop(s_timer);
    s_slot = AWG_SLOT_COUNT;
    s_idle = true;
    emitSilence(AWG_R_RANGE_DROP);
    return;
  }

  if (!s_outputOn) {
    awgTxCtrl(CTRL_ACTIVE, AWG_R_CTRL_ON, seq);
    s_outputOn = true;
  }

  /* 新 B0 到来即重置节拍：取消挂起的定时器，从 slot0 重新起 */
  if (s_timer) esp_timer_stop(s_timer);
  s_idle = false;
  s_slot = 0;
  pushSlot(0);
  esp_timer_start_once(s_timer, AWG_SLOT_US);
}

void schedAbort(void) {
  if (s_timer) esp_timer_stop(s_timer);
  s_slot = AWG_SLOT_COUNT;
  s_seq = 0xFF;
  s_idle = true;
  emitSilence(AWG_R_BLE_DROP);   /* §8：断开 ≤10ms 内静默，无条件执行 */
}

void schedPoll(void) {
#if AWG_SPI_ENABLE
  if (awgFrameFaulted()) {
    /* §4 要点 6：重试仍失败 → 静默并停机等待人工复位 */
    if (s_timer) esp_timer_stop(s_timer);
    enterIdle(AWG_R_FAULT);
    return;
  }
#endif
  if (!s_idle && (uint32_t)(millis() - s_lastB0Ms) > AWG_B0_TIMEOUT_MS) {
    /* 兜底：esp_timer 未能推进（异常场景）时仍按 §8 静默 */
    if (s_timer) esp_timer_stop(s_timer);
    enterIdle(AWG_R_IDLE_SILENCE);
  }
}

bool schedIsIdle(void) { return s_idle; }
uint8_t schedCurrentSlot(void) { return s_slot; }
