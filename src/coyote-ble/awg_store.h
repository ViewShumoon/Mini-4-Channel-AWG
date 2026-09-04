/*
 * awg_store.h - BF 参数的 NVS 断电保存（§2.4）
 *
 * 键名 namespace "awg" + softA/softB/bal1A/bal1B/bal2A/bal2B，
 * 首次上电无记录时取默认 soft=200, bal1=128, bal2=128。
 */
#ifndef AWG_STORE_H
#define AWG_STORE_H

#include <Arduino.h>

typedef struct {
  uint8_t soft[2];
  uint8_t bal1[2];
  uint8_t bal2[2];
} AwgStoredParams;

/* 载入（缺失时填默认值），返回 true 表示 NVS 中已有历史记录 */
bool awgStoreLoad(AwgStoredParams *p);

/* 写回；内部只在值变化时落盘，避免 flash 磨损 */
void awgStoreSave(const AwgStoredParams *p);

#endif /* AWG_STORE_H */
