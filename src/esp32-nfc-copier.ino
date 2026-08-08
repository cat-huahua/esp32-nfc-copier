/*
 * ESP32 NFC 卡复制器 (MIFARE Classic 1K) + Gen1a 后门 + Flash 存储
 * --------------------------------------------------
 * 硬件:
 *   ESP32 DevKit + RC522(MFRC522, SPI) + DST-013 1.3" OLED(SH1106, I2C) + 2 按键
 *
 * 依赖库(Arduino IDE -> 库管理器):
 *   - "MFRC522" by GithubCommunity / miguelbalboa
 *   - "U8g2"     by oliver
 *   - Preferences (ESP32 core 自带)
 *
 * 按键:
 *   BTN1 (GPIO32) = 切换菜单 / 上一项
 *   BTN2 (GPIO33) = 确认 / 执行
 *
 * 菜单:
 *   INFO        只显示卡 UID / 类型
 *   READ        读源卡全部 16 扇区到内存
 *   WRITE Gen2  标准写(magic Gen2 卡才能写 UID)
 *   WRITE Gen1a 后门写,可写 0 块 + 密钥块,整卡克隆
 *   SAVE        把内存缓冲存到 flash 槽位
 *   LOAD        从 flash 槽位读回内存
 *
 * 注意: 仅支持默认密钥 FFFFFFFFFFFF 的 MIFARE Classic 1K。
 */

#include <SPI.h>
#include <MFRC522.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>

// ---------- 引脚定义 ----------
#define RC522_SS   5    // RC522 SDA/SS
#define RC522_RST  27   // RC522 RST
// RC522 SCK=18, MOSI=23, MISO=19 (ESP32 VSPI 默认)

#define BTN1_PIN   32   // 切换菜单
#define BTN2_PIN   33   // 确认执行
// OLED I2C: SDA=21, SCL=22 (ESP32 默认)

// ---------- 对象 ----------
MFRC522 mfrc522(RC522_SS, RC522_RST);
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
// SSD1306 屏改用: U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
MFRC522::MIFARE_Key key;
Preferences prefs;

// ---------- 数据缓冲(支持 MIFARE Classic Mini/1K/4K, 扇区/块数动态) ----------
#define MAX_BLOCKS  256      // 4K = 256 块
#define MAX_SECTORS 40       // 4K = 40 扇区
uint8_t cardData[MAX_BLOCKS][16];
bool    blockValid[MAX_BLOCKS];      // 该块是否有效数据(读成功)
int     cardBlocks  = 64;            // 当前卡实际块数
int     cardSectors = 16;            // 当前卡实际扇区数
bool    hasData = false;
uint8_t srcUid[10];
uint8_t srcUidLen = 0;

// MIFARE Classic 扇区几何: 4K 的后 8 个扇区(32..39)每个 16 块, 其余 4 块
static inline int secBlockCount(int s) { return (s < 32) ? 4 : 16; }
static inline int secFirstBlock(int s) { return (s < 32) ? s * 4 : 128 + (s - 32) * 16; }
static inline int secTrailer(int s)    { return secFirstBlock(s) + secBlockCount(s) - 1; }
static inline int blockSector(int b)   { return (b < 128) ? b / 4 : 32 + (b - 128) / 16; }
static inline bool isTrailer(int b)    { return b == secTrailer(blockSector(b)); }

// 按卡类型设置几何; 返回 false = 不支持
bool setGeometry(MFRC522::PICC_Type t) {
  if (t == MFRC522::PICC_TYPE_MIFARE_MINI) { cardSectors = 5;  cardBlocks = 20;  return true; }
  if (t == MFRC522::PICC_TYPE_MIFARE_1K)   { cardSectors = 16; cardBlocks = 64;  return true; }
  if (t == MFRC522::PICC_TYPE_MIFARE_4K)   { cardSectors = 40; cardBlocks = 256; return true; }
  return false;
}

// ---------- 密钥字典 + 每扇区已知密钥 ----------
// 两级: 前面是最常用的出厂/公开密钥, 后面是流传较广的「不那么常用」泄露密钥。
// 按顺序逐个试, 常见卡几个就命中; 试错密钥只是白费一次认证, 不会损坏卡。
const uint8_t KEY_DICT[][6] = {
  // --- 常用 ---
  {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},   // 出厂默认
  {0x00,0x00,0x00,0x00,0x00,0x00},   // 全零
  {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5},   // MAD key A
  {0xD3,0xF7,0xD3,0xF7,0xD3,0xF7},   // NDEF 公钥
  {0xB0,0xB1,0xB2,0xB3,0xB4,0xB5},
  {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF},
  {0x4D,0x3A,0x99,0xC3,0x51,0xDD},
  {0x1A,0x98,0x2C,0x7E,0x45,0x9A},
  // --- 不那么常用(流传的泄露/厂商密钥) ---
  {0x71,0x4C,0x5C,0x88,0x6E,0x97},
  {0x58,0x7E,0xE5,0xF9,0x35,0x0F},
  {0xA0,0x47,0x8C,0xC3,0x90,0x91},
  {0x53,0x3C,0xB6,0xC7,0x23,0xF6},
  {0x8F,0xD0,0xA4,0xF2,0x56,0xE9},
  {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5},
  {0xD0,0xD1,0xD2,0xD3,0xD4,0xD5},
  {0x50,0x4B,0x50,0x4B,0x50,0x4B},
  {0x00,0x00,0x00,0x00,0x00,0x01},
  {0xA1,0xB1,0xC1,0xD1,0xE1,0xF1},
  {0xA0,0xB0,0xC0,0xD0,0xE0,0xF0},
  {0x11,0x22,0x33,0x44,0x55,0x66},
  {0x12,0x34,0x56,0x78,0x9A,0xBC},
  {0xF1,0x23,0x45,0x67,0x89,0xAB},
  {0x00,0x00,0x0F,0xFF,0xFF,0xFF},
  {0x27,0x13,0xFE,0xEC,0x14,0x87},
  {0x22,0x22,0x22,0x22,0x22,0x22},
  {0x33,0x33,0x33,0x33,0x33,0x33},
  {0x66,0x6F,0x6F,0x64,0x00,0x00},
  {0xFC,0x00,0x01,0x87,0x78,0xF7},
  {0x6C,0xA7,0x62,0xBE,0x3E,0xEF},
};
const int NUM_DICT = sizeof(KEY_DICT) / 6;

MFRC522::MIFARE_Key sectorKey[MAX_SECTORS];   // 每扇区找到的可用密钥
uint8_t sectorKeyType[MAX_SECTORS];           // KEY_A / KEY_B
bool    sectorKeyKnown[MAX_SECTORS];          // 该扇区是否已找到密钥

// 运行时「收割」到的候选密钥: 有些卡把下个扇区的密钥明文存在
// 上个扇区的数据块里(key 链), 读通一个扇区就从其数据里抓候选来试后面扇区。
const int MAX_EXTRA = 48;
MFRC522::MIFARE_Key extraKeys[MAX_EXTRA];
int numExtra = 0;

// 卡镜像的每扇区密钥(READ 后由 sectorKey 拷入; 属于 buffer, 供克隆/存档/dump 用)
MFRC522::MIFARE_Key imgKey[MAX_SECTORS];
uint8_t imgKeyType[MAX_SECTORS];
bool    imgKeyKnown[MAX_SECTORS];

// ---------- 菜单 ----------
enum Menu { MENU_INFO, MENU_READ, MENU_W_G2, MENU_W_G1A, MENU_SAVE, MENU_LOAD, MENU_DELETE, MENU_BRIDGE, MENU_COUNT };
const char* menuNames[MENU_COUNT] = {
  "INFO / UID", "READ card", "WRITE Gen2", "WRITE Gen1a",
  "SAVE to flash", "LOAD from flash", "DELETE saved", "PC BRIDGE (USB)"
};
int menuIndex = 0;

// ---------- 按键去抖 ----------
struct Button { uint8_t pin; bool last; uint32_t t; };
Button b1{BTN1_PIN, HIGH, 0};
Button b2{BTN2_PIN, HIGH, 0};

bool pressed(Button &b) {
  bool now = digitalRead(b.pin);
  if (now != b.last && (millis() - b.t) > 30) {
    b.t = millis();
    b.last = now;
    if (now == LOW) return true;   // 接 GND, 按下为 LOW
  }
  return false;
}

// ================= OLED =================
void showLines(const char* l1, const char* l2 = "", const char* l3 = "",
               const char* l4 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, l1);
  u8g2.drawStr(0, 26, l2);
  u8g2.drawStr(0, 40, l3);
  u8g2.drawStr(0, 54, l4);
  u8g2.sendBuffer();
}

void drawMenu() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(0, 6, "== NFC COPIER ==");
  u8g2.drawStr(84, 6, hasData ? "[DATA]" : "[--]");
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = 14 + i * 7;
    if (i == menuIndex) u8g2.drawStr(0, y, ">");
    u8g2.drawStr(7, y, menuNames[i]);
  }
  u8g2.sendBuffer();
}

String uidToStr(uint8_t *uid, uint8_t len) {
  String s;
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
    if (i < len - 1) s += " ";
  }
  s.toUpperCase();
  return s;
}

// ================= 卡检测 =================
bool waitForCard(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) return true;
    delay(50);
  }
  return false;
}

// ================= Gen1a 后门 =================
// 发送 0x40(7bit) + 0x43(8bit) 解锁 magic Gen1a 卡的后门模式
// 成功后可对任意块(含 0 块、密钥块)直接读写,无需认证
bool gen1aUnlock() {
  MFRC522::StatusCode st;
  byte resp;
  byte respLen;
  byte validBits;

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  // 1) 0x40, 7 个有效位
  byte cmd40 = 0x40;
  respLen = 1; validBits = 7;
  st = mfrc522.PCD_TransceiveData(&cmd40, 1, &resp, &respLen, &validBits, 0, false);
  if (st != MFRC522::STATUS_OK) return false;   // 不是 Gen1a 卡 / 无响应

  // 2) 0x43, 8 个有效位
  byte cmd43 = 0x43;
  respLen = 1; validBits = 0;
  st = mfrc522.PCD_TransceiveData(&cmd43, 1, &resp, &respLen, &validBits, 0, false);
  if (st != MFRC522::STATUS_OK) return false;

  return true;
}

// ================= READ =================
// 重新选中当前卡(试下一个密钥前必须重激活, 否则认证失败后卡会 halt)
bool cardReselect() {
  mfrc522.PCD_StopCrypto1();
  byte atqa[2]; byte sz = sizeof(atqa);
  if (mfrc522.PICC_WakeupA(atqa, &sz) != MFRC522::STATUS_OK) return false;
  return mfrc522.PICC_Select(&(mfrc522.uid), 0) == MFRC522::STATUS_OK;
}

// 把一个候选 6 字节密钥加入 extraKeys(去重)
void addExtraKey(const uint8_t *kb) {
  if (numExtra >= MAX_EXTRA) return;
  for (int i = 0; i < numExtra; i++)
    if (memcmp(extraKeys[i].keyByte, kb, 6) == 0) return;  // 已有
  memcpy(extraKeys[numExtra].keyByte, kb, 6);
  numExtra++;
}

// 从某扇区已读到的数据里收割候选密钥:
// - 数据块(j=0,1,2): 前 6 / 后 6 字节(密钥常明文藏在最后一个数据块,如下个扇区密钥)
// - 尾块(j=3): 前 6 = KeyA 字段, 后 6 = KeyB 字段(尾块可读时直接就是密钥)
void harvestKeys(int sector) {
  int f = secFirstBlock(sector), n = secBlockCount(sector);
  for (int j = 0; j < n; j++) {
    int blk = f + j;
    if (!blockValid[blk]) continue;
    addExtraKey(&cardData[blk][0]);            // 前 6 字节
    addExtraKey(&cardData[blk][10]);           // 后 6 字节(尾块=KeyB)
  }
}

// 用一个密钥试某扇区(KEY A 再 KEY B), 成功则记录并保持认证态。
// 返回 1 成功 / 0 失败 / -1 卡不见了。
int tryKeyOnSector(int sector, const uint8_t *kb) {
  int trailer = secTrailer(sector);
  MFRC522::MIFARE_Key k;
  for (int b = 0; b < 6; b++) k.keyByte[b] = kb[b];
  for (int kt = 0; kt < 2; kt++) {
    uint8_t keyType = (kt == 0) ? MFRC522::PICC_CMD_MF_AUTH_KEY_A
                                : MFRC522::PICC_CMD_MF_AUTH_KEY_B;
    if (!cardReselect()) return -1;
    if (mfrc522.PCD_Authenticate(keyType, trailer, &k, &(mfrc522.uid))
        == MFRC522::STATUS_OK) {
      sectorKey[sector] = k;
      sectorKeyType[sector] = keyType;
      sectorKeyKnown[sector] = true;
      return 1;
    }
  }
  return 0;
}

// 找某扇区的可用密钥: 先字典, 再收割到的候选密钥。
// 成功保持认证态返回 >=0; 卡不见了 -1; 全失败 -2。
int findSectorKey(int sector) {
  for (int ki = 0; ki < NUM_DICT; ki++) {
    int r = tryKeyOnSector(sector, KEY_DICT[ki]);
    if (r == 1) return ki;
    if (r == -1) return -1;
  }
  for (int ei = 0; ei < numExtra; ei++) {
    int r = tryKeyOnSector(sector, extraKeys[ei].keyByte);
    if (r == 1) return NUM_DICT + ei;
    if (r == -1) return -1;
  }
  return -2;
}

bool readCard() {
  showLines("READ:", "Tap SOURCE card...");
  if (!waitForCard(10000)) { showLines("READ FAILED", "no card (timeout)"); delay(1500); return false; }

  srcUidLen = mfrc522.uid.size;
  memcpy(srcUid, mfrc522.uid.uidByte, srcUidLen);

  MFRC522::PICC_Type type = mfrc522.PICC_GetType(mfrc522.uid.sak);
  if (!setGeometry(type)) {
    showLines("Unsupported card", String(MFRC522::PICC_GetTypeName(type)).c_str(),
              "Need MIFARE Classic");
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
    delay(2500);
    return false;
  }

  for (int i = 0; i < cardSectors; i++) sectorKeyKnown[i] = false;
  for (int i = 0; i < cardBlocks; i++) blockValid[i] = false;
  numExtra = 0;
  bool sectorDone[MAX_SECTORS];
  for (int i = 0; i < cardSectors; i++) sectorDone[i] = false;

  // 读一个已认证扇区的所有块 + 收割候选密钥(供两轮复用)
  #define READ_SECTOR_BLOCKS(sec)                                        \
    do {                                                                 \
      int f = secFirstBlock(sec), n = secBlockCount(sec);               \
      for (int j = 0; j < n; j++) {                                      \
        int blk = f + j;                                                 \
        uint8_t buf[18]; uint8_t size = sizeof(buf);                     \
        if (mfrc522.MIFARE_Read(blk, buf, &size) == MFRC522::STATUS_OK) {\
          memcpy(cardData[blk], buf, 16); blockValid[blk] = true;        \
        }                                                                \
      }                                                                  \
      harvestKeys(sec); sectorDone[sec] = true;                          \
    } while (0)

  // 第一轮: 顺序读。读通就收割候选密钥供后续扇区用。
  for (int sector = 0; sector < cardSectors; sector++) {
    char l2[24]; snprintf(l2, sizeof(l2), "sector %d/%d (keys)", sector + 1, cardSectors);
    showLines("Reading...", l2);
    int ki = findSectorKey(sector);
    if (ki == -1) {
      showLines("Card removed!", "put it back & retry");
      mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
      delay(2000);
      return false;
    }
    if (ki < 0) continue;                    // 本轮解不开, 留给补轮
    READ_SECTOR_BLOCKS(sector);
  }

  // 补轮: 用新收割的密钥再试未解开的扇区, 直到一轮无进展(key 链逐段解开)
  bool progress = true;
  while (progress) {
    progress = false;
    for (int sector = 0; sector < cardSectors; sector++) {
      if (sectorDone[sector]) continue;
      int ki = findSectorKey(sector);
      if (ki == -1) {
        showLines("Card removed!", "put it back & retry");
        mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
        delay(2000);
        return false;
      }
      if (ki < 0) continue;
      showLines("Recovered key!", "reading locked sec");
      READ_SECTOR_BLOCKS(sector);
      progress = true;                       // 有进展就再来一轮
    }
  }
  #undef READ_SECTOR_BLOCKS

  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();

  // 把发现的密钥拷入卡镜像(供克隆尾块回填 / 存档 / dump)
  for (int s = 0; s < cardSectors; s++) {
    imgKeyKnown[s] = sectorKeyKnown[s];
    imgKeyType[s]  = sectorKeyType[s];
    imgKey[s]      = sectorKey[s];
  }

  // 统计
  int okBlocks = 0, failBlocks = 0, lockedSectors = 0;
  for (int i = 0; i < cardBlocks; i++) (blockValid[i] ? okBlocks : failBlocks)++;
  for (int s = 0; s < cardSectors; s++) if (!sectorDone[s]) lockedSectors++;

  hasData = (okBlocks > 0);
  char l2[24], l3[24], l4[24];
  snprintf(l2, sizeof(l2), "OK:%d FAIL:%d", okBlocks, failBlocks);
  String uidS = uidToStr(srcUid, srcUidLen);
  snprintf(l3, sizeof(l3), "UID %s", uidS.c_str());
  if (lockedSectors > 0)
    snprintf(l4, sizeof(l4), "%d sec: key unknown", lockedSectors);
  else
    l4[0] = '\0';
  showLines(hasData ? "READ DONE" : "READ FAILED", l2, l3, l4);
  delay(3000);
  return hasData;
}

// ================= WRITE (Gen2 标准) =================
bool writeCardGen2() {
  if (!hasData) { showLines("No data in buffer", "READ/LOAD first"); delay(2000); return false; }
  showLines("WRITE Gen2:", "Tap TARGET card...");
  if (!waitForCard(10000)) { showLines("WRITE FAILED", "no card (timeout)"); delay(1500); return false; }

  int okBlocks = 0, failBlocks = 0, skipped = 0;
  for (int sector = 0; sector < cardSectors; sector++) {
    // 用字典在目标卡上找该扇区可用密钥(空白卡默认 FF 一次就中)
    int ki = findSectorKey(sector);
    if (ki < 0) { failBlocks += secBlockCount(sector); continue; }

    int f = secFirstBlock(sector), n = secBlockCount(sector);
    for (int j = 0; j < n; j++) {
      int blk = f + j;
      if (isTrailer(blk))   { skipped++; continue; }     // 跳过密钥块,防锁死
      if (!blockValid[blk]) { skipped++; continue; }     // 无效数据不写
      if (mfrc522.MIFARE_Write(blk, cardData[blk], 16) == MFRC522::STATUS_OK) okBlocks++;
      else failBlocks++;                                  // 块0在普通卡失败属正常
    }
    char l2[24]; snprintf(l2, sizeof(l2), "sector %d/%d", sector + 1, cardSectors);
    showLines("Writing...", l2);
  }
  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();

  char l2[24], l3[24];
  snprintf(l2, sizeof(l2), "OK:%d FAIL:%d", okBlocks, failBlocks);
  snprintf(l3, sizeof(l3), "trailer skip:%d", skipped);
  showLines("WRITE DONE", l2, l3, "verify on reader!");
  delay(3000);
  return true;
}

// 组装写入克隆卡的尾块: 读回来的 KeyA 恒为屏蔽的 00, 这里回填发现的真密钥,
// 让克隆卡能用和源卡相同的密钥被读出。访问位/KeyB 沿用读到的。
void buildTrailer(int sector, uint8_t out[16]) {
  int trailer = secTrailer(sector);
  memcpy(out, cardData[trailer], 16);            // 访问位[6..9] + KeyB[10..15] 用读到的
  if (imgKeyKnown[sector]) {
    if (imgKeyType[sector] == MFRC522::PICC_CMD_MF_AUTH_KEY_A) {
      memcpy(out, imgKey[sector].keyByte, 6);    // 回填真 KeyA
      bool zb = true; for (int i = 10; i < 16; i++) if (out[i]) { zb = false; break; }
      if (zb) memcpy(out + 10, imgKey[sector].keyByte, 6);  // KeyB 不可读(全0)时兜底
    } else {
      memcpy(out + 10, imgKey[sector].keyByte, 6);  // 回填真 KeyB
      memcpy(out, imgKey[sector].keyByte, 6);       // KeyA 未知, 用已知 key 兜底
    }
  }
}

// ================= WRITE (Gen1a 后门, 整卡克隆) =================
bool writeCardGen1a() {
  if (!hasData) { showLines("No data in buffer", "READ/LOAD first"); delay(2000); return false; }
  showLines("WRITE Gen1a:", "Tap magic card...");
  if (!waitForCard(10000)) { showLines("WRITE FAILED", "no card (timeout)"); delay(1500); return false; }

  if (!gen1aUnlock()) {
    showLines("Gen1a unlock FAIL", "not a Gen1a card?", "try WRITE Gen2");
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
    delay(2500);
    return false;
  }
  showLines("Gen1a unlocked", "writing ALL blocks");
  delay(500);

  // 后门模式下无需认证, 直接写全部有效块(含块0与密钥块 => 整卡克隆)
  // 尾块用 buildTrailer 回填真密钥, 使克隆卡密钥 = 源卡
  int okBlocks = 0, failBlocks = 0, skipped = 0;
  for (int blk = 0; blk < cardBlocks; blk++) {
    if (!blockValid[blk]) { skipped++; continue; }
    uint8_t tmp[16];
    const uint8_t *src;
    if (isTrailer(blk)) { buildTrailer(blockSector(blk), tmp); src = tmp; }  // 尾块回填密钥
    else src = cardData[blk];
    if (mfrc522.MIFARE_Write(blk, (byte *)src, 16) == MFRC522::STATUS_OK) okBlocks++;
    else failBlocks++;

    if (isTrailer(blk)) {
      char l2[24]; snprintf(l2, sizeof(l2), "block %d/%d", blk + 1, cardBlocks);
      showLines("Cloning...", l2);
    }
  }
  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();

  char l2[24], l3[24];
  snprintf(l2, sizeof(l2), "OK:%d FAIL:%d", okBlocks, failBlocks);
  snprintf(l3, sizeof(l3), "skip:%d (no data)", skipped);
  showLines("CLONE DONE", l2, l3, "UID copied!");
  delay(3000);
  return true;
}

// ================= INFO =================
void showCardInfo() {
  showLines("INFO:", "Tap a card...");
  if (!waitForCard(8000)) { showLines("no card", "(timeout)"); delay(1500); return; }
  MFRC522::PICC_Type type = mfrc522.PICC_GetType(mfrc522.uid.sak);
  String uidS = uidToStr(mfrc522.uid.uidByte, mfrc522.uid.size);
  char l3[24]; snprintf(l3, sizeof(l3), "SAK 0x%02X len %d", mfrc522.uid.sak, mfrc522.uid.size);
  showLines(String(MFRC522::PICC_GetTypeName(type)).c_str(), uidS.c_str(), l3);
  mfrc522.PICC_HaltA();
  delay(4000);
}

// ================= Flash 动态存储(卡数动态, 存/删后自动重算) =================
// 每张卡: cI_img(压缩镜像,含密钥) cI_uid cI_ulen cI_sz ; "count" = 已存卡数
void keyFor(char *out, int idx, const char *suffix) {
  snprintf(out, 16, "c%d_%s", idx, suffix);
}
int cardCount() {
  prefs.begin("nfc", true);
  int n = prefs.getInt("count", 0);
  prefs.end();
  return n;
}
String storeUidStr(int idx) {
  char k[16]; keyFor(k, idx, "ulen");
  prefs.begin("nfc", true);
  uint8_t ln = prefs.getUChar(k, 0);
  uint8_t tmp[10]; keyFor(k, idx, "uid"); prefs.getBytes(k, tmp, ln);
  prefs.end();
  return uidToStr(tmp, ln);
}
uint16_t storeSize(int idx) {
  char k[16]; keyFor(k, idx, "sz");
  prefs.begin("nfc", true);
  uint16_t s = prefs.getUShort(k, 0);
  prefs.end();
  return s;
}

// ---------- PackBits 游程压缩(连续相同字节折叠成 个数×值) ----------
// 控制字节 n: 0..127 = 后面 n+1 个字面字节; 129..255 = 把下一个字节重复 257-n 次;
// 128 保留。既压缩连续 00/33 等, 又不会让随机密文块膨胀。
size_t packbitsEncode(const uint8_t *src, size_t len, uint8_t *dst) {
  size_t i = 0, o = 0;
  while (i < len) {
    size_t run = 1;
    while (i + run < len && src[i + run] == src[i] && run < 128) run++;
    if (run >= 2) {                              // 重复段
      dst[o++] = (uint8_t)(257 - run);
      dst[o++] = src[i];
      i += run;
    } else {                                     // 字面段
      size_t start = i, lit = 0;
      while (i < len && lit < 128) {
        if (i + 1 < len && src[i + 1] == src[i]) break;  // 遇到重复就停
        i++; lit++;
      }
      dst[o++] = (uint8_t)(lit - 1);
      memcpy(dst + o, src + start, lit); o += lit;
    }
  }
  return o;
}

size_t packbitsDecode(const uint8_t *src, size_t len, uint8_t *dst, size_t maxDst) {
  size_t i = 0, o = 0;
  while (i < len) {
    uint8_t n = src[i++];
    if (n < 128) {                               // 字面 n+1 个
      size_t cnt = n + 1;
      if (o + cnt > maxDst || i + cnt > len) break;
      memcpy(dst + o, src + i, cnt); o += cnt; i += cnt;
    } else if (n > 128) {                         // 重复 257-n 次
      size_t cnt = 257 - n;
      if (o + cnt > maxDst || i >= len) break;
      memset(dst + o, src[i++], cnt); o += cnt;
    }
  }
  return o;
}

// 把卡镜像序列化: 版本 + 几何 + UID(不压) + 每扇区密钥(不压) + 块数据(PackBits 压缩)。
// 块明文流 = 每块 [valid(1)][16 数据], cardBlocks*17 字节, 再整体 PackBits。
size_t serializeCard(uint8_t *out) {
  size_t p = 0;
  out[p++] = 0x04;                               // 版本(动态几何)
  out[p++] = (uint8_t)cardSectors;               // 扇区数
  out[p++] = (uint8_t)(cardBlocks & 0xFF);       // 块数低字节
  out[p++] = (uint8_t)(cardBlocks >> 8);         // 块数高字节(256 需要)
  out[p++] = srcUidLen;
  for (int i = 0; i < srcUidLen; i++) out[p++] = srcUid[i];
  for (int s = 0; s < cardSectors; s++) {        // 每扇区密钥(不压)
    out[p++] = imgKeyKnown[s] ? 1 : 0;
    out[p++] = imgKeyType[s];
    for (int b = 0; b < 6; b++) out[p++] = imgKeyKnown[s] ? imgKey[s].keyByte[b] : 0;
  }
  static uint8_t plain[MAX_BLOCKS * 17];
  size_t pp = 0;
  for (int blk = 0; blk < cardBlocks; blk++) {
    plain[pp++] = blockValid[blk] ? 1 : 0;
    memcpy(plain + pp, cardData[blk], 16); pp += 16;
  }
  p += packbitsEncode(plain, pp, out + p);       // 压缩块数据
  return p;
}

bool deserializeCard(const uint8_t *in, size_t len) {
  size_t p = 0;
  if (len < 5 || in[p++] != 0x04) return false;
  cardSectors = in[p++];
  cardBlocks  = in[p++] | (in[p++] << 8);
  if (cardSectors > MAX_SECTORS || cardBlocks > MAX_BLOCKS) return false;
  srcUidLen = in[p++];
  if (srcUidLen > 10 || p + srcUidLen > len) return false;
  for (int i = 0; i < srcUidLen; i++) srcUid[i] = in[p++];
  for (int s = 0; s < cardSectors; s++) {
    if (p + 8 > len) return false;
    imgKeyKnown[s] = in[p++] != 0;
    imgKeyType[s]  = in[p++];
    for (int b = 0; b < 6; b++) imgKey[s].keyByte[b] = in[p++];
  }
  static uint8_t plain[MAX_BLOCKS * 17];
  size_t pp = packbitsDecode(in + p, len - p, plain, sizeof(plain));
  if (pp != (size_t)cardBlocks * 17) return false;
  size_t q = 0;
  for (int blk = 0; blk < cardBlocks; blk++) {
    blockValid[blk] = plain[q++] != 0;
    memcpy(cardData[blk], plain + q, 16); q += 16;
  }
  hasData = true;
  return true;
}

// 追加保存当前 buffer 为新卡; 返回索引, -1 = 失败(NVS 满?)
int saveCardNew() {
  static uint8_t buf[5000];
  size_t n = serializeCard(buf);
  int idx = cardCount();
  char k[16];
  prefs.begin("nfc", false);
  keyFor(k, idx, "img");
  size_t w = prefs.putBytes(k, buf, n);
  bool ok = (w == n);
  if (ok) {
    keyFor(k, idx, "uid");  prefs.putBytes(k, srcUid, srcUidLen);
    keyFor(k, idx, "ulen"); prefs.putUChar(k, srcUidLen);
    keyFor(k, idx, "sz");   prefs.putUShort(k, (uint16_t)n);
    prefs.putInt("count", idx + 1);
  } else {
    keyFor(k, idx, "img"); prefs.remove(k);          // 回滚半写
  }
  prefs.end();
  return ok ? idx : -1;
}

bool loadCard(int idx) {
  if (idx < 0 || idx >= cardCount()) return false;
  static uint8_t buf[5000];
  char k[16]; keyFor(k, idx, "img");
  prefs.begin("nfc", true);
  size_t len = prefs.getBytesLength(k);
  bool ok = (len > 0 && len <= sizeof(buf));
  if (ok) prefs.getBytes(k, buf, len);
  prefs.end();
  if (!ok) return false;
  return deserializeCard(buf, len);
}

void removeCardKeys(int idx) {
  char k[16];
  keyFor(k, idx, "img");  prefs.remove(k);
  keyFor(k, idx, "uid");  prefs.remove(k);
  keyFor(k, idx, "ulen"); prefs.remove(k);
  keyFor(k, idx, "sz");   prefs.remove(k);
}

// 原地删除 idx: 把最后一张搬到 idx, 删最后一张, count--(动态重算槽数)
bool deleteCard(int idx) {
  int n = cardCount();
  if (idx < 0 || idx >= n) return false;
  int last = n - 1;
  static uint8_t buf[5000];
  prefs.begin("nfc", false);
  if (idx != last) {                                  // 把末尾搬到空洞
    char k[16];
    keyFor(k, last, "img");
    size_t len = prefs.getBytesLength(k);
    if (len > 0 && len <= sizeof(buf)) {
      prefs.getBytes(k, buf, len);
      keyFor(k, idx, "img"); prefs.putBytes(k, buf, len);
    }
    keyFor(k, last, "uid"); uint8_t uid[10];
    size_t ul = prefs.getBytes(k, uid, sizeof(uid));
    keyFor(k, idx, "uid"); prefs.putBytes(k, uid, ul);
    keyFor(k, last, "ulen"); uint8_t ulen = prefs.getUChar(k, 0);
    keyFor(k, idx, "ulen"); prefs.putUChar(k, ulen);
    keyFor(k, last, "sz"); uint16_t sz = prefs.getUShort(k, 0);
    keyFor(k, idx, "sz"); prefs.putUShort(k, sz);
  }
  removeCardKeys(last);
  prefs.putInt("count", n - 1);
  prefs.end();
  return true;
}

// 从已存卡里选一张: B1 切换(末尾 BACK), B2 确认。返回索引 / -1 取消 / -2 无卡
int selectCard(const char *title) {
  int n = cardCount();
  if (n == 0) return -2;
  int sel = 0;                                        // 0..n-1, n = BACK
  bool redraw = true;
  while (true) {
    if (redraw) {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_6x12_tr);
      u8g2.drawStr(0, 11, title);
      if (sel == n) {
        u8g2.drawStr(0, 30, "<< BACK / cancel");
      } else {
        char line[24]; snprintf(line, sizeof(line), "Card %d / %d", sel + 1, n);
        u8g2.drawStr(0, 28, line);
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.drawStr(0, 42, ("UID " + storeUidStr(sel)).c_str());
        char sz[24]; snprintf(sz, sizeof(sz), "%u B compressed", storeSize(sel));
        u8g2.drawStr(0, 52, sz);
      }
      u8g2.setFont(u8g2_font_5x7_tr);
      u8g2.drawStr(0, 63, "B1=next   B2=select");
      u8g2.sendBuffer();
      redraw = false;
    }
    if (pressed(b1)) { sel = (sel + 1) % (n + 1); redraw = true; }
    if (pressed(b2)) return (sel == n) ? -1 : sel;
    delay(10);
  }
}

void doSave() {
  if (!hasData) { showLines("No data in buffer", "READ a card first"); delay(2000); return; }
  int idx = saveCardNew();
  if (idx < 0) { showLines("SAVE FAILED", "NVS full?", "delete some cards"); delay(2500); return; }
  char l2[24]; snprintf(l2, sizeof(l2), "saved as #%d", idx + 1);
  char l3[24]; snprintf(l3, sizeof(l3), "total %d cards", cardCount());
  showLines("SAVED", l2, l3, ("UID " + uidToStr(srcUid, srcUidLen)).c_str());
  delay(2000);
}

void doLoad() {
  int idx = selectCard("LOAD which card?");
  if (idx == -2) { showLines("No saved cards", "SAVE one first"); delay(1800); return; }
  if (idx < 0) return;
  if (!loadCard(idx)) { showLines("LOAD FAILED", "corrupt slot?"); delay(1800); return; }
  char l2[24]; snprintf(l2, sizeof(l2), "loaded #%d", idx + 1);
  showLines("LOADED", l2, ("UID " + uidToStr(srcUid, srcUidLen)).c_str(), "buffer ready");
  delay(2000);
}

void doDelete() {
  int idx = selectCard("DELETE which?");
  if (idx == -2) { showLines("No saved cards", "nothing to delete"); delay(1800); return; }
  if (idx < 0) return;
  String u = storeUidStr(idx);
  bool decided = false, yes = false, drawn = false;
  while (!decided) {
    if (!drawn) { showLines("DELETE card?", ("UID " + u).c_str(), "B2=YES  B1=NO"); drawn = true; }
    if (pressed(b2)) { yes = true; decided = true; }
    if (pressed(b1)) { yes = false; decided = true; }
    delay(10);
  }
  if (!yes) return;
  deleteCard(idx);
  char l2[24]; snprintf(l2, sizeof(l2), "%d cards left", cardCount());
  showLines("DELETED", l2, "slots recounted");
  delay(1800);
}

// ================= PC BRIDGE (USB 串口读卡器) =================
// 经典 ESP32 无原生 USB, 无法当 PC/SC CCID 读卡器。
// 这里用 USB 串口(115200)做命令桥接, 配套 host/nfc_host.py 使用。
// 命令(每行一条, \n 结尾):
//   PING              -> PONG
//   UID               -> "UID <hex>"  或 "ERR NOCARD"
//   INFO              -> "INFO <type>|<uid>"
//   RBLK <n>          -> "BLK <n> <32hex>"  (默认密钥 A 认证)
//   WBLK <n> <32hex>  -> "OK" 或 "ERR ..."
//   DUMP              -> 逐块 "BLK nn <hex>" ... 然后 "DONE"
// 设备端按 BTN1 退出该模式。

bool bridgeActivate() {
  return mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial();
}

void bridgePrintHex(uint8_t *d, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) {
    if (d[i] < 0x10) Serial.print('0');
    Serial.print(d[i], HEX);
  }
}

int bridgeNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c |= 0x20;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool bridgeParseHex16(const String &s, uint8_t *out) {
  if (s.length() != 32) return false;
  for (int i = 0; i < 16; i++) {
    int hi = bridgeNib(s[2 * i]), lo = bridgeNib(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (hi << 4) | lo;
  }
  return true;
}

void bridgeReadBlk(int blk) {
  if (blk < 0 || blk >= MAX_BLOCKS) { Serial.println("ERR RANGE"); return; }
  if (!bridgeActivate())            { Serial.println("ERR NOCARD"); return; }
  int trailer = secTrailer(blockSector(blk));
  if (mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key,
                               &(mfrc522.uid)) != MFRC522::STATUS_OK) {
    Serial.println("ERR AUTH");
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
    return;
  }
  uint8_t buf[18], sz = sizeof(buf);
  if (mfrc522.MIFARE_Read(blk, buf, &sz) == MFRC522::STATUS_OK) {
    Serial.print("BLK "); Serial.print(blk); Serial.print(' ');
    bridgePrintHex(buf, 16); Serial.println();
  } else {
    Serial.println("ERR READ");
  }
  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
}

void bridgeWriteBlk(int blk, uint8_t *data) {
  if (blk < 0 || blk >= MAX_BLOCKS) { Serial.println("ERR RANGE"); return; }
  if (!bridgeActivate())            { Serial.println("ERR NOCARD"); return; }
  int trailer = secTrailer(blockSector(blk));
  if (mfrc522.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key,
                               &(mfrc522.uid)) != MFRC522::STATUS_OK) {
    Serial.println("ERR AUTH");
    mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
    return;
  }
  if (mfrc522.MIFARE_Write(blk, data, 16) == MFRC522::STATUS_OK) Serial.println("OK");
  else Serial.println("ERR WRITE");
  mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
}

// DUMP 用完整字典 + 密钥收割读卡(复用 readCard 的逻辑), 输出 UID + KEY + BLK。
// KEY 行含发现的每扇区密钥, 供电脑端存档/写回时回填尾块。
void bridgeDump() {
  if (!readCard()) { Serial.println("ERR READ"); return; }   // 填充 cardData/imgKey/blockValid
  Serial.print("UID "); bridgePrintHex(srcUid, srcUidLen); Serial.println();
  for (int s = 0; s < cardSectors; s++) {
    Serial.print("KEY "); if (s < 10) Serial.print('0'); Serial.print(s); Serial.print(' ');
    if (imgKeyKnown[s]) {
      Serial.print(imgKeyType[s] == MFRC522::PICC_CMD_MF_AUTH_KEY_A ? 'A' : 'B');
      Serial.print(' '); bridgePrintHex(imgKey[s].keyByte, 6);
    } else {
      Serial.print("- ------------");
    }
    Serial.println();
  }
  for (int blk = 0; blk < cardBlocks; blk++) {
    Serial.print("BLK "); if (blk < 10) Serial.print('0'); Serial.print(blk); Serial.print(' ');
    if (blockValid[blk]) bridgePrintHex(cardData[blk], 16);
    else Serial.print("--------------INVALID-----------");
    Serial.println();
  }
  Serial.println("DONE");
}

void bridgeHandle(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  String C = cmd; C.toUpperCase();

  if (C == "PING") { Serial.println("PONG"); return; }
  if (C == "HELP") { Serial.println("CMDS: PING UID INFO RBLK <n> WBLK <n> <hex32> DUMP"); return; }
  if (C == "UID") {
    if (!bridgeActivate()) { Serial.println("ERR NOCARD"); return; }
    Serial.print("UID "); bridgePrintHex(mfrc522.uid.uidByte, mfrc522.uid.size); Serial.println();
    mfrc522.PICC_HaltA();
    return;
  }
  if (C == "INFO") {
    if (!bridgeActivate()) { Serial.println("ERR NOCARD"); return; }
    MFRC522::PICC_Type t = mfrc522.PICC_GetType(mfrc522.uid.sak);
    Serial.print("INFO "); Serial.print(MFRC522::PICC_GetTypeName(t)); Serial.print('|');
    bridgePrintHex(mfrc522.uid.uidByte, mfrc522.uid.size); Serial.println();
    mfrc522.PICC_HaltA();
    return;
  }
  if (C == "DUMP") { bridgeDump(); return; }
  if (C.startsWith("RBLK")) { bridgeReadBlk(cmd.substring(4).toInt()); return; }
  if (C.startsWith("WBLK")) {
    int s1 = cmd.indexOf(' ');
    int s2 = cmd.indexOf(' ', s1 + 1);
    if (s1 < 0 || s2 < 0) { Serial.println("ERR ARGS"); return; }
    int blk = cmd.substring(s1 + 1, s2).toInt();
    String hx = cmd.substring(s2 + 1); hx.trim();
    uint8_t data[16];
    if (!bridgeParseHex16(hx, data)) { Serial.println("ERR HEX"); return; }
    bridgeWriteBlk(blk, data);
    return;
  }
  Serial.println("ERR UNKNOWN");
}

void pcBridgeMode() {
  showLines("PC BRIDGE (USB)", "serial @115200", "run nfc_host.py", "BTN1 = exit");
  // 清空可能残留的串口输入
  while (Serial.available()) Serial.read();
  Serial.println("READY nfc-bridge v1");

  String line = "";
  uint32_t count = 0;
  while (true) {
    if (pressed(b1)) { Serial.println("BYE"); return; }
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\r') continue;
      if (c == '\n') {
        bridgeHandle(line);
        line = "";
        count++;
        char l2[24]; snprintf(l2, sizeof(l2), "cmds: %lu", (unsigned long)count);
        showLines("PC BRIDGE active", l2, "listening...", "BTN1 = exit");
      } else if (line.length() < 90) {
        line += c;
      }
    }
    delay(2);
  }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  pinMode(BTN1_PIN, INPUT_PULLUP);
  pinMode(BTN2_PIN, INPUT_PULLUP);

  u8g2.begin();
  SPI.begin();               // SCK18 MISO19 MOSI23
  mfrc522.PCD_Init();

  for (uint8_t i = 0; i < 6; i++) key.keyByte[i] = 0xFF;  // 默认密钥
  for (int i = 0; i < MAX_BLOCKS; i++) blockValid[i] = false;

  // --- RC522 实时自检: 循环读版本寄存器, 按 BTN2 继续进菜单 ---
  Serial.println("RC522 live self-check. Wiggle wires; BTN2 to continue.");
  bool go = false;
  while (!go) {
    mfrc522.PCD_Init();                       // 每轮重初始化, 重插线能立即恢复
    byte ver = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
    bool rcOk = !(ver == 0x00 || ver == 0xFF);
    char l2[24]; snprintf(l2, sizeof(l2), "RC522 ver: 0x%02X", ver);
    showLines("RC522 self-check", l2,
              rcOk ? "*** SPI OK! ***" : "NO SPI - wiggle",
              "BTN2 = continue");
    Serial.print("VersionReg=0x"); Serial.println(ver, HEX);
    for (int i = 0; i < 10 && !go; i++) {     // ~500ms, 同时探按键
      if (pressed(b2)) go = true;
      delay(50);
    }
  }
  drawMenu();
}

void loop() {
  if (pressed(b1)) { menuIndex = (menuIndex + 1) % MENU_COUNT; drawMenu(); }
  if (pressed(b2)) {
    switch (menuIndex) {
      case MENU_INFO:   showCardInfo();   break;
      case MENU_READ:   readCard();       break;
      case MENU_W_G2:   writeCardGen2();  break;
      case MENU_W_G1A:  writeCardGen1a(); break;
      case MENU_SAVE:   doSave();         break;
      case MENU_LOAD:   doLoad();         break;
      case MENU_DELETE: doDelete();       break;
      case MENU_BRIDGE: pcBridgeMode();   break;
    }
    drawMenu();
  }
}
