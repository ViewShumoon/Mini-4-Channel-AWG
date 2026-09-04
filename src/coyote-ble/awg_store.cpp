/*
 * awg_store.cpp - 见 awg_store.h
 */
#include "awg_store.h"
#include <Preferences.h>

#define AWG_NVS_NS     "awg"
#define AWG_NVS_SOFT_A "softA"
#define AWG_NVS_SOFT_B "softB"
#define AWG_NVS_B1_A   "bal1A"
#define AWG_NVS_B1_B   "bal1B"
#define AWG_NVS_B2_A   "bal2A"
#define AWG_NVS_B2_B   "bal2B"

static Preferences s_prefs;
static bool s_open = false;
static AwgStoredParams s_lastSaved;

bool awgStoreLoad(AwgStoredParams *p) {
  s_open = s_prefs.begin(AWG_NVS_NS, false);
  bool hadHistory = false;
  /* 默认值：soft 满量程 200、平衡参数居中 128 */
  p->soft[0] = p->soft[1] = 200;
  p->bal1[0] = p->bal1[1] = 128;
  p->bal2[0] = p->bal2[1] = 128;
  if (!s_open) {
    s_lastSaved = *p;
    return false;
  }
  if (s_prefs.isKey(AWG_NVS_SOFT_A)) { p->soft[0] = s_prefs.getUChar(AWG_NVS_SOFT_A, 200); hadHistory = true; }
  if (s_prefs.isKey(AWG_NVS_SOFT_B)) { p->soft[1] = s_prefs.getUChar(AWG_NVS_SOFT_B, 200); hadHistory = true; }
  if (s_prefs.isKey(AWG_NVS_B1_A))   { p->bal1[0] = s_prefs.getUChar(AWG_NVS_B1_A, 128); hadHistory = true; }
  if (s_prefs.isKey(AWG_NVS_B1_B))   { p->bal1[1] = s_prefs.getUChar(AWG_NVS_B1_B, 128); hadHistory = true; }
  if (s_prefs.isKey(AWG_NVS_B2_A))   { p->bal2[0] = s_prefs.getUChar(AWG_NVS_B2_A, 128); hadHistory = true; }
  if (s_prefs.isKey(AWG_NVS_B2_B))   { p->bal2[1] = s_prefs.getUChar(AWG_NVS_B2_B, 128); hadHistory = true; }
  s_lastSaved = *p;
  return hadHistory;
}

static void putIfChanged(const char *key, uint8_t val, uint8_t *cached) {
  if (val == *cached) return;
  s_prefs.putUChar(key, val);
  *cached = val;
}

void awgStoreSave(const AwgStoredParams *p) {
  if (!s_open) return;
  putIfChanged(AWG_NVS_SOFT_A, p->soft[0], &s_lastSaved.soft[0]);
  putIfChanged(AWG_NVS_SOFT_B, p->soft[1], &s_lastSaved.soft[1]);
  putIfChanged(AWG_NVS_B1_A,   p->bal1[0], &s_lastSaved.bal1[0]);
  putIfChanged(AWG_NVS_B1_B,   p->bal1[1], &s_lastSaved.bal1[1]);
  putIfChanged(AWG_NVS_B2_A,   p->bal2[0], &s_lastSaved.bal2[0]);
  putIfChanged(AWG_NVS_B2_B,   p->bal2[1], &s_lastSaved.bal2[1]);
}
