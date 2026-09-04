/*
 * awg_scheduler.h - 25ms 槽位排程器（§4）+ 安全静默（§8）
 *
 *   收到有效 B0 → 装载 sched[2][4] → slot_idx=0 → 立即推送 slot0 → esp_timer 起 25ms
 *   esp_timer 到期 → slot_idx++ → 推送；slot_idx==4 → 进 IDLE 并静默
 *   4 槽播完即静默，不做循环重播；300ms 无有效 B0 → 兜底静默
 */
#ifndef AWG_SCHEDULER_H
#define AWG_SCHEDULER_H

#include <Arduino.h>
#include "coyote_proto.h"

/* 创建 esp_timer 与日志队列（不含 BLE） */
void schedInit(void);

/* 上电握手：读 ID → CTRL=静默 → WDT_PRESET → 清波形寄存器 */
void schedBoot(void);

/* 收到有效 B0：装载 4 槽并立即推送 slot0（以 B0 到达沿为槽 0 起点） */
void schedLoad(const AwgSlot sched[2][AWG_SLOT_COUNT], uint8_t seq, bool anyValid);

/* BLE 断开：立即静默（§8 时限 ≤10ms） */
void schedAbort(void);

/* loop 中调用：B0 流超时兜底 + 故障锁存检查 */
void schedPoll(void);

/* 诊断 */
bool schedIsIdle(void);
uint8_t schedCurrentSlot(void);

#endif /* AWG_SCHEDULER_H */
