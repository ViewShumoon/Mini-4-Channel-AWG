/*
 * coyote-ble.ino - ESP32-C3 BLE 外设侧：coyote/v3 协议（"脉冲主机 3.0"）
 *
 * 规范来源: documents/esp32-spi-fpga-link-v3.md （§2 BLE / §3 映射 / §4 排程 / §6 帧 / §7 寄存器 / §8 安全）
 * 协议原文: DG-LAB-OPENSOURCE  coyote/v3/README_V3.md
 *
 * 本期定位（AWG_SPI_ENABLE = 0）：不接 FPGA，固件照常生成"本应通过 SPI 发给 FPGA"的
 * 每一帧，并把它们以十六进制 + 触发原因标签从 UART 打印出来，供人工校验。
 * 接上 FPGA 后把 awg_config.h 里的 AWG_SPI_ENABLE 改成 1 即可走真实 SPI，逻辑不变。
 *
 * 依赖:
 *   - Arduino ESP32 core（含 NimBLE 栈）
 *   - NimBLE-Arduino by h2zero，≥ 2.0（回调签名带 NimBLEConnInfo&）
 *   板卡: ESP32C3 Dev Module（或 "Adafruit QTPy ESP32-C3" 等 C3 板），
 *   需在 Boards Menu 选含 BLE 的 ESP32-C3 目标；本文件不占用 USB-CDC 之外的外设，
 *   AWG_SPI_ENABLE=0 时 GPIO4/5/6/7/10 均不被驱动。
 */

#include <NimBLEDevice.h>

#include "awg_config.h"
#include "awg_store.h"
#include "coyote_proto.h"
#include "fpga_frame.h"
#include "awg_scheduler.h"

/* ------------------------------------------------------------------ */
/* 全局状态                                                             */
/* ------------------------------------------------------------------ */
static CoyoteState     g_state;
static AwgStoredParams g_params;
static NimBLECharacteristic *g_pB1 = nullptr;   /* 0x150B NOTIFY */
static uint32_t g_b0Count = 0;
static uint32_t g_bfCount = 0;

/*
 * B1 延迟发送：NimBLE 回调运行在 host 任务上下文，在其内部直接
 * notify() 会重入 host 锁（h2zero 官方禁止）。onWrite 只暂存内容，
 * 实际通知在 loop() 中发出。连续多条带 seq 的 B0 只回告最新 seq
 * （§2.3：APP 建议等待，协议未禁止以新命令覆盖）。
 */
static volatile bool g_b1Pending = false;
static uint8_t g_b1Data[BLE_B1_LEN];

/* ------------------------------------------------------------------ */
/* BLE 回调                                                            */
/* ------------------------------------------------------------------ */
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    (void)pServer;
    (void)connInfo;
    Serial.println(F("== BLE connected (wait B0/BF)"));
  }

  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo,
                    int reason) override {
    (void)pServer;
    (void)connInfo;
    /* §8: BLE 断开 → ≤10ms 内写 CTRL 关输出 + 两通道 AMP=0 */
    schedAbort();
    Serial.printf("== BLE disconnected reason=%d -> SILENCE emitted, "
                  "keep advertising\r\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

class HeartbeatCallbacks : public NimBLECharacteristicCallbacks {
  /* 0x150A: APP 每 100ms 写入 B0（20B）或一次性写入 BF（7B） */
  void onWrite(NimBLECharacteristic *pChr, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    std::string v = pChr->getValue();
    const uint8_t *d = (const uint8_t *)v.data();
    uint8_t len = (uint8_t)v.size();
    if (len == 0) return;

    if (d[0] == 0xB0) {
      AwgSlot sched[2][AWG_SLOT_COUNT];
      uint8_t flags = coyoteDecodeB0(&g_state, d, len, sched);
      if (flags & COYOTE_B0_BAD_LEN) {
        Serial.printf("!! B0 rejected: len=%u (need %d)\r\n", len, BLE_B0_LEN);
        return;
      }
      g_b0Count++;

      /* 原文回显，便于人工对照"收到的 B0"与"算出的帧" */
      Serial.printf("-- B0 #%lu seq=%u mA=%u mB=%u sA=%u sB=%u\r\n",
                    (unsigned long)g_b0Count, (unsigned)((d[1] >> 4) & 0x0F),
                    (unsigned)((d[1] >> 2) & 0x03), (unsigned)(d[1] & 0x03),
                    g_state.strength[0], g_state.strength[1]);
      Serial.print(F("   A f:"));
      for (uint8_t i = 0; i < 4; i++) Serial.printf(" %3u", d[4 + i]);
      Serial.print(F("  a:"));
      for (uint8_t i = 0; i < 4; i++) Serial.printf(" %3u", d[8 + i]);
      Serial.print(F("   |  B f:"));
      for (uint8_t i = 0; i < 4; i++) Serial.printf(" %3u", d[12 + i]);
      Serial.print(F("  a:"));
      for (uint8_t i = 0; i < 4; i++) Serial.printf(" %3u", d[16 + i]);
      Serial.printf("   => A %s / B %s\r\n",
                    (flags & COYOTE_B0_A_VALID) ? "VALID" : "DROP(all 4)",
                    (flags & COYOTE_B0_B_VALID) ? "VALID" : "DROP(all 4)");
      /* 逐槽换算结果（phase_inc / amp12），人工校验的主对照表 */
      for (uint8_t c = 0; c < 2; c++) {
        for (uint8_t i = 0; i < AWG_SLOT_COUNT; i++) {
          Serial.printf("   %c[%u] wire=%3u phase=%10lu amp12=%4u %s\r\n",
                        'A' + c, i, sched[c][i].freqWire,
                        (unsigned long)sched[c][i].phaseInc, sched[c][i].amp12,
                        sched[c][i].enable ? "" : "(silent)");
        }
      }

      bool anyValid = (flags & (COYOTE_B0_A_VALID | COYOTE_B0_B_VALID)) != 0;
      schedLoad(sched, (uint8_t)((d[1] >> 4) & 0x0F), anyValid);

      /* §2.3 seq 契约：seq != 0 必须回 B1，否则 APP 的 isInputAllowed 永久卡死 */
      if (flags & COYOTE_B0_NEED_B1) {
        coyoteBuildB1(&g_state, (uint8_t)((d[1] >> 4) & 0x0F), g_b1Data);
        g_b1Pending = true;   /* 通知本体在 loop() 中发出，见上方说明 */
      }
      return;
    }

    if (d[0] == 0xBF) {
      uint8_t changed = coyoteDecodeBF(&g_state, d, len);
      g_bfCount++;
      Serial.printf("-- BF #%lu len=%u raw=", (unsigned long)g_bfCount, len);
      for (uint8_t i = 0; i < len && i < BLE_BF_LEN; i++)
        Serial.printf("%02X ", d[i]);
      Serial.printf("| softA=%u softB=%u bal1A=%u bal1B=%u bal2A=%u bal2B=%u",
                    g_state.soft[0], g_state.soft[1], g_state.bal1[0],
                    g_state.bal1[1], g_state.bal2[0], g_state.bal2[1]);
      if (changed) {
        g_params.soft[0] = g_state.soft[0];
        g_params.soft[1] = g_state.soft[1];
        g_params.bal1[0] = g_state.bal1[0];
        g_params.bal1[1] = g_state.bal1[1];
        g_params.bal2[0] = g_state.bal2[0];
        g_params.bal2[1] = g_state.bal2[1];
        awgStoreSave(&g_params);          /* §2.4 断电保存 */
        /* bal 参数写入诊断寄存器（不参与波形计算，CFG_USE_BAL=0） */
        awgTxWriteReg(REG_FREQ_WIRE0, 0xFFFF0000u |
                      ((uint32_t)g_state.bal1[0] << 8) | g_state.bal2[0],
                      AWG_R_CONFIG, 0xFF);
      }
      return;                                   /* BF 无回告 */
    }

    Serial.printf("!! unknown HEAD 0x%02X len=%u, ignored\r\n", d[0], len);
  }
};

class BatteryCallbacks : public NimBLECharacteristicCallbacks {
  /* 0x1500: 固定回报 0x64（无电池，常量即可） */
  void onRead(NimBLECharacteristic *pChr, NimBLEConnInfo &connInfo) override {
    (void)connInfo;
    static const uint8_t b = BLE_BATTERY_VALUE;
    pChr->setValue(&b, 1);
  }
};

static ServerCallbacks      g_serverCb;
static HeartbeatCallbacks   g_hbCb;
static BatteryCallbacks     g_batCb;

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */
void setup() {
  Serial.begin(AWG_LOG_BAUD);
  delay(300);

  Serial.println();
  Serial.println(F("=== coyote/v3 ESP32-C3 peripheral ==="));
  Serial.printf("adv name    : %s\r\n", BLE_DEVICE_NAME);
  Serial.printf("GATT        : 0x180C{0x150A W, 0x150B N} 0x180A{0x1500 R}\r\n");
  Serial.printf("F_CLK_DDS   : %lu Hz (must equal awg_platform.F_CLK_DDS)\r\n",
                (unsigned long)F_CLK_DDS);
  Serial.printf("FREQ_DIRECT : %d (1 = wire byte is Hz 10..240)\r\n",
                AWG_FREQ_DIRECT);
  Serial.printf("USE_BAL     : %d\r\n", CFG_USE_BAL);
  Serial.printf("SPI OUTPUT  : %s\r\n",
                AWG_SPI_ENABLE ? "REAL SPI (4MHz Mode0)" :
                                 "UART HEX ONLY (frames not driven on SPI pins)");

  /* 持久化参数 + 协议状态 */
  bool hadHistory = awgStoreLoad(&g_params);
  coyoteInit(&g_state, g_params.soft, g_params.bal1, g_params.bal2);
  Serial.printf("NVS         : %s soft=%u/%u bal1=%u/%u bal2=%u/%u\r\n",
                hadHistory ? "restored" : "defaults",
                g_params.soft[0], g_params.soft[1], g_params.bal1[0],
                g_params.bal1[1], g_params.bal2[0], g_params.bal2[1]);

  /* 帧通道 + 排程器 */
  awgFrameInit();
  schedInit();
  schedBoot();
  awgFramePump();

  /* --- GATT 表（§2.1）：广播名由 NimBLEDevice::init() 设定并默认随广播包发出 --- */
  NimBLEDevice::init(BLE_DEVICE_NAME);

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&g_serverCb);

  NimBLEService *pHb = pServer->createService(BLE_UUID_SERVICE_HEARTBEAT);
  NimBLECharacteristic *pW = pHb->createCharacteristic(
      BLE_UUID_CHAR_WRITE, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR,
      BLE_B0_LEN);
  pW->setCallbacks(&g_hbCb);
  g_pB1 = pHb->createCharacteristic(BLE_UUID_CHAR_NOTIFY,
                                    NIMBLE_PROPERTY::NOTIFY, BLE_B0_LEN);

  NimBLEService *pDev = pServer->createService(BLE_UUID_SERVICE_DEVICE);
  NimBLECharacteristic *pBat = pDev->createCharacteristic(
      BLE_UUID_CHAR_BATTERY, NIMBLE_PROPERTY::READ, 1);
  pBat->setCallbacks(&g_batCb);
  /* 协议原文该特征为"读/通知"，本设计文档 §2.1 只列 READ，此处按文档实现 READ */

  pServer->start();          /* 服务随 server 一起 start */

  NimBLEAdvertising *pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(BLE_UUID_SERVICE_HEARTBEAT);
  pAdv->addServiceUUID(BLE_UUID_SERVICE_DEVICE);
  pAdv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
  Serial.println(F("== advertising, scan for \"47L121000\""));
  Serial.println(F("--- frames below: each line is one SPI frame + reason tag ---"));
}

/* ------------------------------------------------------------------ */
/* loop：消费帧队列（UART 打印必须留在任务上下文，不能占用 25ms 节拍）      */
/* ------------------------------------------------------------------ */
void loop() {
  /* B1 通知必须离开 NimBLE host 回调上下文后再发（见 g_b1Pending 注释） */
  if (g_b1Pending) {
    g_b1Pending = false;
    g_pB1->setValue(g_b1Data, BLE_B1_LEN);
    g_pB1->notify();
    g_state.pendingSeq = 0;
    Serial.printf("<- B1 %02X %02X %02X %02X (seq=%u A=%u B=%u)\r\n",
                  g_b1Data[0], g_b1Data[1], g_b1Data[2], g_b1Data[3],
                  g_b1Data[1], g_b1Data[2], g_b1Data[3]);
  }
  schedPoll();
  awgFramePump();
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last > 5000) {
    last = now;
    Serial.printf("[stat] B0=%lu BF=%lu dropped=%lu idle=%d slot=%u "
                  "sA=%u sB=%u\r\n",
                  (unsigned long)g_b0Count, (unsigned long)g_bfCount,
                  (unsigned long)awgFrameDroppedCount(),
                  (int)schedIsIdle(), (unsigned)schedCurrentSlot(),
                  g_state.strength[0], g_state.strength[1]);
  }
  delay(1);
}
