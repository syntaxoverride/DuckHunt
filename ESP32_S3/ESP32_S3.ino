/*
 * WiCyS Game — ESP32-S3 (Waveshare ESP32-S3-Touch-AMOLED-1.8)
 *
 * Phone: nRF Connect → DUCK-<COLOUR> → Read STORY → Write SUBMIT (Text) → Read FLAG
 * Side buttons: BOOT=next colour, PWR=prev colour
 * Swipe LEFT → Admin (PIN 24650): reset bobbers, Game 1 / Game 2 (syncs to all ducks)
 * Screen: duck only — scenario lives in STORY
 *
 * CYD port lives in ../CYD/
 */
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "Arduino_DriveBus_Library.h"
#include <Adafruit_XCA9554.h>
#include "pin_config.h"
#include "HWCDC.h"
#include <Wire.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include "XPowersLib.h"
#include <math.h>
#include <ctype.h>
#include <string.h>

HWCDC USBSerial;
Adafruit_XCA9554 expander;
static bool s_expander_ok = false;
XPowersPMU power;
static bool s_pmu_ok = false;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);
Arduino_Canvas *cv = nullptr;

std::shared_ptr<Arduino_IIC_DriveBus> IIC_Bus =
  std::make_shared<Arduino_HWIIC>(IIC_SDA, IIC_SCL, &Wire);

void Arduino_IIC_Touch_Interrupt(void);
std::unique_ptr<Arduino_IIC> CST816(new Arduino_CST816x(
  IIC_Bus, CST816T_DEVICE_ADDRESS, DRIVEBUS_DEFAULT_VALUE, TP_INT,
  Arduino_IIC_Touch_Interrupt));
void Arduino_IIC_Touch_Interrupt(void) { CST816->IIC_Interrupt_Flag = true; }

#define SCR_W LCD_WIDTH
#define SCR_H LCD_HEIGHT
#define TARGET_FPS 30
#define FRAME_MS   (1000 / TARGET_FPS)

#define COL_SKY        RGB565_BLACK
#define COL_WATER      0x09CB
#define COL_DEEP       0x0947
#define COL_CREST      0x3C99
#define COL_RIPPLE     0x8EBF
#define COL_LABEL      0xC618
#define COL_MUTE       0x528A
#define COL_TALLY      0xF800
#define COL_TALLY_LOW  0xFFFF
#define COL_TALLY_ROD  0xC618
#define COL_TALLY_SEAM 0x0000
#define COL_ADMIN      0x3B9F
#define COL_ADMIN_DK   0x1B3D
#define COL_AMBER      0xFDA0
#define COL_AMBER_DK   0x9200
#define COL_OK_BG      0x0B45
#define COL_INK        0xFFFF

enum { PX_NONE = 0, PX_Y, PX_y, PX_O, PX_K, PX_COUNT };
static uint16_t PAL_CALM[PX_COUNT] = { 0, 0xFE89, 0xE545, 0xFC45, 0x10A3 };
static uint16_t PAL_HOT [PX_COUNT] = { 0, 0xFF98, 0xFED1, 0xFE31, 0x3942 };
static uint16_t PAL_REFL_BODY[PX_COUNT];
static uint16_t PAL_REFL_DEEP[PX_COUNT];

typedef struct {
  const char *name;
  uint16_t body, shade, beak;
} tier_t;

static const tier_t TIERS[] = {
  { "yellow", 0xFE89, 0xE545, 0 },
  { "green",  0x3626, 0x1B85, 0 },
  { "blue",   0x2D7F, 0x1B3D, 0 },
  { "orange", 0xFC66, 0xCB23, 0xBA82 },
  { "red",    0xF9A6, 0xC0C4, 0 },
  { "black",  0x4A69, 0x2965, 0 },
};
#define N_TIERS (sizeof(TIERS) / sizeof(TIERS[0]))
static uint8_t s_tier = 0;
static Preferences s_prefs;
#define TIER (&TIERS[s_tier])

#define SPR_COLS 16
#define SPR_ROWS 13
#define SPR_CELL 16
#define DUCK_OX  ((SCR_W - SPR_COLS * SPR_CELL) / 2)
#define DUCK_OY  56
#define DUCK_CX  (DUCK_OX + (SPR_COLS * SPR_CELL) / 2)
#define DUCK_CY  (DUCK_OY + (SPR_ROWS * SPR_CELL) / 2)

static const char *DUCK_SPR[SPR_ROWS] = {
  ".....YYYY.......", "....YYYYYY......", "....YYKYYY......",
  "....YYYYYYOOO...", "....YYYYYYOOO...", ".....YYYYY......",
  ".....YYYY.......", "..YYYYYYYYY..YY.", ".YYYYYYYYYYYYYY.",
  "YYYYYYYYYYYYYYY.", "YyyyyyyyyyyyyyY.", ".YyyyyyyyyyyyY..",
  "..YYYYYYYYYYY..."
};

#define ADV_INTERVAL_MS  100
#define ADV_PER_PULSE      5
#define PULSE_MS         (ADV_INTERVAL_MS * ADV_PER_PULSE)
#define RING_LIFE_MS    1500
#define N_RINGS          (RING_LIFE_MS / PULSE_MS)
#define RING_R0           28
#define RING_R1          220
#define RING_LEVEL      0.38f
#define BLINK_PERIOD_MS 10000
#define BLINK_MS          180
#define RIPPLE_RATE     0.6f
#define WATER_Y          280
#define WATER_DEPTH_Y    340
#define WAVE_COL_W         4
#define WAVE_A1          3.2f
#define WAVE_A2          1.8f
#define BOB_R             18
#define TALLY_MAX          8
#define TALLY_X0          28
#define TALLY_DX          42

#define BOOT_PIN           0
#define PWR_EXIO           4
#define BTN_DEBOUNCE_MS  35
#define BTN_COOL_MS     280
#define REACT_MS        900
#define WRONG_COOL_MS   800
#define ADMIN_CODE       "24650"
#define SWIPE_MIN_DX      70
#define SWIPE_MAX_CROSS   90
#define SYNC_BROADCAST_MS 20000
#define SCAN_EVERY_MS     4000
#define SCAN_WINDOW_MS     900

#define BLE_SVC_UUID    "d0c40010-0000-1000-8000-00805f9b34fb"
#define BLE_STORY_UUID  "d0c40011-0000-1000-8000-00805f9b34fb"
#define BLE_SUBMIT_UUID "d0c40012-0000-1000-8000-00805f9b34fb"
#define BLE_FLAG_UUID   "d0c40013-0000-1000-8000-00805f9b34fb"
#define BLE_STATUS_UUID "d0c40014-0000-1000-8000-00805f9b34fb"

/* Manufacturer data: C4 D0 | cmd | game   cmd=1 means "switch to this game" */
#define MD0 0xC4
#define MD1 0xD0
#define MD_CMD_SET 0x01

#define N_STAGES 3
#define N_GAMES  2

typedef struct {
  const char *title;
  const char *story[N_STAGES];
  const char *answer[N_STAGES];
  const char *flag[N_STAGES];
  const char *colour_need; /* stage 3 flock colour, or nullptr */
  const char *done;
} game_def_t;

static const game_def_t GAMES[N_GAMES] = {
  {
    "Game 1",
    {
      "Hi! I am a duck. Write QUACK to SUBMIT as Text (not Hex). Then Read FLAG.",
      "Map scrambled: UE9ORA== (Base64). Decode to one word. Write it to SUBMIT as Text.",
      "Only YELLOW feathers trust me. Use side buttons, then write HOME to SUBMIT as Text."
    },
    { "QUACK", "POND", "HOME" },
    { "WiCyS{first_quack}", "WiCyS{follow_pond}", "WiCyS{yellow_home}" },
    "yellow",
    "You helped me get home. Nice work! Your FLAG is still in FLAG. WiCyS ducks + BLE."
  },
  {
    "Game 2",
    {
      "Round 2! Write WADDLE to SUBMIT as Text (not Hex). Then Read FLAG.",
      "Snack code: QlJFQUQ= (Base64). Decode to one word. Write it to SUBMIT as Text.",
      "Only GREEN feathers trust me. Use side buttons, then write NEST to SUBMIT as Text."
    },
    { "WADDLE", "BREAD", "NEST" },
    { "WiCyS{g2_waddle}", "WiCyS{g2_bread}", "WiCyS{g2_green_nest}" },
    "green",
    "Round 2 complete. Nice flock work! Your FLAG is still in FLAG."
  },
};

#define GAME (&GAMES[s_game])

typedef enum {
  MODE_IDLE, MODE_ADMIN_GATE, MODE_ADMIN_PIN, MODE_ADMIN_MENU,
} ui_mode_t;

#define ROW_H  58
#define ROW_X  16
#define ROW_W  (SCR_W - 32)
#define ROW_Y0 78
#define KEY_W 108
#define KEY_H 54
#define KEY_X0 18
#define KEY_Y0 98
#define KEY_GAP 5

static const char *PIN_KEYS[4][3] = {
  { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "<", "0", "OK" },
};

static int16_t  s_surf[SCR_W / WAVE_COL_W];
static char     s_duck_name[20] = "DUCK-YELLOW";
static bool     s_ble_ok = false;
static bool     s_ble_started = false;
static bool     s_angry = false;
static bool     s_happy = false;
static uint32_t s_react_until = 0;
static uint8_t  s_bobbers[N_TIERS] = {0};
static uint8_t  s_stage = 0;
static uint8_t  s_game = 0;
static char     s_status[24] = "WAITING";
static char     s_flag_buf[40] = "";
static uint32_t s_wrong_cool = 0;
static uint32_t s_sync_until = 0;
static uint32_t s_next_scan = 0;

static ui_mode_t s_mode = MODE_IDLE;
static bool     s_ui_dirty = true;
static char     s_pin[8];
static uint8_t  s_pin_len = 0;
static int8_t   s_key_held = -1;
static int8_t   s_btn_held = -1;
static bool     s_down = false;
static int16_t  s_tx0, s_ty0, s_tx, s_ty;
static uint32_t s_tdown = 0;
static uint32_t s_gest_cool = 0;

static BLECharacteristic *s_ch_story  = nullptr;
static BLECharacteristic *s_ch_flag   = nullptr;
static BLECharacteristic *s_ch_status = nullptr;
static BLEScan *s_scan = nullptr;

static bool     s_boot_down = false;
static bool     s_pwr_down  = false;
static uint32_t s_boot_change = 0;
static uint32_t s_pwr_change  = 0;
static uint32_t s_btn_cool    = 0;

static void apply_game(uint8_t g, bool broadcast);
static void ble_restart_adv(void);
static void ble_publish(void);

static uint16_t blend565(uint16_t bg, uint16_t fg, uint8_t alpha)
{
  int br = (bg >> 11) & 0x1F, bgn = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  int fr = (fg >> 11) & 0x1F, fgn = (fg >> 5) & 0x3F, fb = fg & 0x1F;
  int r = (fr * alpha + br * (255 - alpha)) / 255;
  int g = (fgn * alpha + bgn * (255 - alpha)) / 255;
  int b = (fb * alpha + bb * (255 - alpha)) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint8_t spr_index(char c)
{
  switch (c) {
    case 'Y': return PX_Y; case 'y': return PX_y;
    case 'O': return PX_O; case 'K': return PX_K;
    default:  return PX_NONE;
  }
}

static void apply_tier_palette(void)
{
  PAL_CALM[PX_Y] = TIER->body;
  PAL_CALM[PX_y] = TIER->shade;
  PAL_HOT[PX_Y]  = blend565(TIER->body,  0xFFFF, 110);
  PAL_HOT[PX_y]  = blend565(TIER->shade, 0xFFFF, 90);
  if (TIER->beak) {
    PAL_CALM[PX_O] = TIER->beak;
    PAL_HOT[PX_O]  = blend565(TIER->beak, 0xFFFF, 80);
  } else {
    PAL_CALM[PX_O] = 0xFC45;
    PAL_HOT[PX_O]  = 0xFE31;
  }
  for (int i = 1; i < PX_COUNT; i++) {
    PAL_REFL_BODY[i] = blend565(COL_WATER, PAL_CALM[i], 41);
    PAL_REFL_DEEP[i] = blend565(COL_DEEP,  PAL_CALM[i], 41);
  }
}

static void build_duck_name(void)
{
  snprintf(s_duck_name, sizeof s_duck_name, "DUCK-%s", TIER->name);
  for (char *p = s_duck_name + 5; *p; ++p) *p = (char)toupper((unsigned char)*p);
}

static void reset_progress(void)
{
  s_stage = 0;
  s_flag_buf[0] = 0;
  strncpy(s_status, "WAITING", sizeof s_status);
  memset(s_bobbers, 0, sizeof s_bobbers);
}

static void apply_game(uint8_t g, bool broadcast)
{
  if (g >= N_GAMES) return;
  s_game = g;
  s_prefs.putUChar("game", s_game);
  reset_progress();
  ble_publish();
  if (broadcast) {
    s_sync_until = millis() + SYNC_BROADCAST_MS;
    USBSerial.printf("admin: broadcast %s to flock\n", GAME->title);
  }
  ble_restart_adv();
  USBSerial.printf("game -> %s\n", GAME->title);
}

static void normalize_answer(char *dst, size_t dst_sz, const uint8_t *data, size_t len)
{
  size_t o = 0;
  /* Skip UTF-8 BOM */
  size_t i0 = 0;
  if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i0 = 3;
  for (size_t i = i0; i < len && o + 1 < dst_sz; i++) {
    char c = (char)data[i];
    if (c == '\0') continue; /* allow UTF-16 style padding */
    if (!isalnum((unsigned char)c)) continue;
    dst[o++] = (char)toupper((unsigned char)c);
  }
  dst[o] = 0;
}

static void on_submit(const uint8_t *data, size_t len)
{
  uint32_t now = millis();
  if ((int32_t)(now - s_wrong_cool) < 0) return;

  char ans[40];
  normalize_answer(ans, sizeof ans, data, len);
  USBSerial.printf("submit: '%s' game=%u stage=%u colour=%s\n",
                   ans, (unsigned)s_game, (unsigned)s_stage, TIER->name);

  if (s_stage >= N_STAGES) {
    strncpy(s_status, "DONE", sizeof s_status);
    ble_publish();
    return;
  }

  if (strcmp(ans, GAME->answer[s_stage]) != 0) {
    strncpy(s_status, "WRONG", sizeof s_status);
    s_wrong_cool = now + WRONG_COOL_MS;
    s_angry = true; s_happy = false; s_react_until = now + REACT_MS;
    ble_publish();
    return;
  }

  if (s_stage == 2 && GAME->colour_need &&
      strcasecmp(TIER->name, GAME->colour_need) != 0) {
    strncpy(s_status, "WRONG_COLOUR", sizeof s_status);
    s_wrong_cool = now + WRONG_COOL_MS;
    s_angry = true; s_happy = false; s_react_until = now + REACT_MS;
    ble_publish();
    return;
  }

  strncpy(s_flag_buf, GAME->flag[s_stage], sizeof s_flag_buf - 1);
  s_flag_buf[sizeof s_flag_buf - 1] = 0;
  strncpy(s_status, "UNLOCKED", sizeof s_status);
  s_happy = true; s_angry = false; s_react_until = now + REACT_MS;
  if (s_bobbers[s_tier] < TALLY_MAX) s_bobbers[s_tier]++;
  USBSerial.printf("stage %u solved -> %s\n", (unsigned)s_stage, s_flag_buf);

  s_stage++;
  if (s_stage >= N_STAGES) strncpy(s_status, "DONE", sizeof s_status);
  ble_publish();
}

class SubmitCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *ch) override
  {
    String v = ch->getValue();
    if (v.length() == 0) return;
    on_submit((const uint8_t *)v.c_str(), (size_t)v.length());
  }
};
static SubmitCallbacks s_submit_cb;

static volatile int8_t s_pending_game = -1;

class GameSyncCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override
  {
    if (!advertisedDevice.haveManufacturerData()) return;
    String md = advertisedDevice.getManufacturerData();
    if (md.length() < 4) return;
    const uint8_t *p = (const uint8_t *)md.c_str();
    if (p[0] != MD0 || p[1] != MD1 || p[2] != MD_CMD_SET) return;
    uint8_t g = p[3];
    if (g >= N_GAMES || g == s_game) return;
    s_pending_game = (int8_t)g;
  }
};
static GameSyncCallbacks s_sync_cb;

static void ble_publish(void)
{
  if (!s_ble_ok || !s_ch_story) return;
  const char *story = (s_stage >= N_STAGES) ? GAME->done : GAME->story[s_stage];
  s_ch_story->setValue(String(story));
  s_ch_flag->setValue(String(s_flag_buf));
  s_ch_status->setValue(String(s_status));
  if (s_ble_started) s_ch_status->notify();
}

static void add_name(BLECharacteristic *ch, const char *name)
{
  BLEDescriptor *d = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
  d->setValue(name);
  ch->addDescriptor(d);
}

static void ble_restart_adv(void)
{
  build_duck_name();
  if (!s_ble_ok) return;
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->stop();
  BLEAdvertisementData ad;
  ad.setName(s_duck_name);
  ad.setCompleteServices(BLEUUID(BLE_SVC_UUID));
  uint8_t md[4] = {
    MD0, MD1,
    (uint8_t)(((int32_t)(millis() - s_sync_until) < 0) ? MD_CMD_SET : 0x00),
    s_game
  };
  /* Build binary manuf data safely (may contain 0x00). */
  String mds;
  mds.reserve(4);
  for (int i = 0; i < 4; i++) mds += (char)md[i];
  ad.setManufacturerData(mds);
  adv->setAdvertisementData(ad);
  BLEAdvertisementData scan;
  scan.setName(s_duck_name);
  adv->setScanResponseData(scan);
  adv->setMinInterval(160);
  adv->setMaxInterval(160);
  adv->start();
  USBSerial.printf("BLE advertising as %s (%s)%s\n",
                   s_duck_name, GAME->title,
                   ((int32_t)(millis() - s_sync_until) < 0) ? " [SYNC]" : "");
}

static void ble_init(void)
{
  build_duck_name();
  BLEDevice::init(s_duck_name);
  BLEDevice::setMTU(517);
  BLEDevice::setPower(ESP_PWR_LVL_N0);
  BLEServer *server = BLEDevice::createServer();
  BLEService *svc = server->createService(BLE_SVC_UUID);

  s_ch_story = svc->createCharacteristic(BLE_STORY_UUID, BLECharacteristic::PROPERTY_READ);
  add_name(s_ch_story, "STORY");
  s_ch_flag = svc->createCharacteristic(BLE_FLAG_UUID, BLECharacteristic::PROPERTY_READ);
  add_name(s_ch_flag, "FLAG");
  s_ch_status = svc->createCharacteristic(
    BLE_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  add_name(s_ch_status, "STATUS");
  BLECharacteristic *submit = svc->createCharacteristic(
    BLE_SUBMIT_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  add_name(submit, "SUBMIT");
  submit->setCallbacks(&s_submit_cb);

  strncpy(s_status, "WAITING", sizeof s_status);
  s_flag_buf[0] = 0;
  s_ble_ok = true;
  s_ble_started = false;
  ble_publish();
  svc->start();
  s_ble_started = true;
  ble_publish();
  ble_restart_adv();

  s_scan = BLEDevice::getScan();
  s_scan->setAdvertisedDeviceCallbacks(&s_sync_cb, true);
  s_scan->setActiveScan(true);
  s_scan->setInterval(100);
  s_scan->setWindow(80);

  String mac = BLEDevice::getAddress().toString().c_str();
  mac.toUpperCase();
  USBSerial.printf("BLE MAC %s | %s\n", mac.c_str(), GAME->title);
}

static void poll_game_sync(uint32_t now)
{
  if (!s_ble_ok || !s_scan) return;

  if (s_pending_game >= 0) {
    uint8_t g = (uint8_t)s_pending_game;
    s_pending_game = -1;
    USBSerial.printf("sync: flock set -> %s\n", GAMES[g].title);
    apply_game(g, false);
  }

  static bool was_sync = false;
  bool syncing = (int32_t)(now - s_sync_until) < 0;
  if (was_sync && !syncing) ble_restart_adv();
  was_sync = syncing;

  if (s_mode != MODE_IDLE) return;
  if ((int32_t)(now - s_next_scan) < 0) return;
  s_next_scan = now + SCAN_EVERY_MS;
  s_scan->start(1, false); /* ~1s scan window */
  s_scan->clearResults();
}

static void set_tier(uint8_t t)
{
  if (t >= N_TIERS) return;
  s_tier = t;
  s_prefs.putUChar("tier", s_tier);
  apply_tier_palette();
  ble_restart_adv();
}

static void cycle_tier(int dir)
{
  int next = (int)s_tier + dir;
  while (next < 0) next += (int)N_TIERS;
  set_tier((uint8_t)(next % (int)N_TIERS));
}

static void draw_ellipse(int cx, int cy, int rx, int ry, uint16_t colour)
{
  for (int i = 0; i < 48; i++) {
    float a = (float)i * 2.0f * (float)PI / 48;
    int x = cx + (int)(rx * cosf(a));
    int y = cy + (int)(ry * sinf(a));
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) cv->drawPixel(x, y, colour);
  }
}

static void draw_water(float t)
{
  for (int i = 0; i < SCR_W / WAVE_COL_W; i++) {
    int x = i * WAVE_COL_W;
    int s = WATER_Y + (int)lroundf(WAVE_A1 * sinf(x / 26.0f + t * 1.6f)
                                 + WAVE_A2 * sinf(x / 11.0f - t * 2.3f));
    s_surf[i] = s;
    cv->fillRect(x, s, WAVE_COL_W, SCR_H - s, COL_WATER);
    int d = s > WATER_DEPTH_Y ? s : WATER_DEPTH_Y;
    if (d < SCR_H) cv->fillRect(x, d, WAVE_COL_W, SCR_H - d, COL_DEEP);
  }
}

static void draw_crests(void)
{
  for (int i = 0; i < SCR_W / WAVE_COL_W; i++)
    cv->fillRect(i * WAVE_COL_W, s_surf[i], WAVE_COL_W, 2, COL_CREST);
}

static void draw_reflection(int bob)
{
  for (int r = 0; r < SPR_ROWS; r++) {
    int y = WATER_Y + (SPR_ROWS - 1 - r) * SPR_CELL + 10 - bob;
    if (y >= SCR_H) continue;
    for (int c = 0; c < SPR_COLS; c++) {
      uint8_t ix = spr_index(DUCK_SPR[r][c]);
      if (ix == PX_NONE) continue;
      uint16_t col = (y >= WATER_DEPTH_Y) ? PAL_REFL_DEEP[ix] : PAL_REFL_BODY[ix];
      cv->fillRect(DUCK_OX + c * SPR_CELL, y, SPR_CELL, SPR_CELL, col);
    }
  }
}

static void draw_duck(int bob, bool hot, bool blink)
{
  const uint16_t *pal = hot ? PAL_HOT : PAL_CALM;
  for (int r = 0; r < SPR_ROWS; r++) {
    for (int c = 0; c < SPR_COLS; c++) {
      uint8_t ix = spr_index(DUCK_SPR[r][c]);
      if (ix == PX_NONE) continue;
      uint16_t col = (ix == PX_K && blink && !s_angry) ? pal[PX_Y] : pal[ix];
      cv->fillRect(DUCK_OX + c * SPR_CELL, DUCK_OY + r * SPR_CELL + bob,
                   SPR_CELL, SPR_CELL, col);
    }
  }
  if (s_angry) {
    int ex = DUCK_OX + 6 * SPR_CELL;
    int ey = DUCK_OY + 2 * SPR_CELL + bob;
    cv->fillRect(ex - 4, ey - 10, SPR_CELL + 20, 5, pal[PX_K]);
  }
}

static void draw_rings(uint32_t now)
{
  for (int i = 0; i < N_RINGS; i++) {
    float ph = fmodf((float)now / RING_LIFE_MS + (float)i / N_RINGS, 1.0f);
    int r = RING_R0 + (int)(ph * (RING_R1 - RING_R0));
    uint8_t g = (uint8_t)(63.0f * RING_LEVEL * (1.0f - ph));
    if (g) cv->drawCircle(DUCK_CX, DUCK_CY, r, (uint16_t)(g << 5));
  }
}

static void draw_tally(uint32_t now)
{
  uint16_t n = s_bobbers[s_tier];
  if (n > TALLY_MAX) n = TALLY_MAX;
  for (uint16_t i = 0; i < n; i++) {
    int x = TALLY_X0 + i * TALLY_DX;
    if (x + BOB_R >= SCR_W) break;
    int surf = s_surf[x / WAVE_COL_W];
    int b = (int)lroundf(sinf(now / 300.0f + i * 0.9f) * 2.0f);
    int cy = surf + b + BOB_R - 6;
    cv->drawFastVLine(x, cy - BOB_R - 7, 7, COL_TALLY_ROD);
    cv->fillCircle(x, cy, BOB_R, COL_TALLY);
    for (int dy = 1; dy <= BOB_R; dy++) {
      int dx = (int)lroundf(sqrtf((float)(BOB_R * BOB_R - dy * dy)));
      cv->drawFastHLine(x - dx, cy + dy, 2 * dx + 1, COL_TALLY_LOW);
    }
    cv->drawFastHLine(x - BOB_R, cy, 2 * BOB_R + 1, COL_TALLY_SEAM);
    cv->fillCircle(x - BOB_R / 3, cy - BOB_R / 2, 2, COL_TALLY_LOW);
  }
}

static void draw_btn(int y, const char *txt, uint16_t bg, uint16_t border)
{
  cv->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 12, bg);
  cv->drawRoundRect(ROW_X, y, ROW_W, ROW_H, 12, border);
  cv->setTextSize(2); cv->setTextColor(COL_INK);
  int tw = (int)strlen(txt) * 12;
  cv->setCursor(ROW_X + (ROW_W - tw) / 2, y + 20);
  cv->print(txt);
}

/* Two-tone blue power line (duck screen + admin). */
static void draw_battery_line(void)
{
  const int y = SCR_H - 10;
  const int x0 = 16;
  const int x1 = SCR_W - 16;
  const int h = 6;
  const uint16_t COL_TRACK = 0x0A2C; /* deep navy */
  const uint16_t COL_FILL  = 0x3B9F; /* brighter blue */

  cv->fillRoundRect(x0, y, x1 - x0, h, 3, COL_TRACK);

  if (!s_pmu_ok || !power.isBatteryConnect()) return;

  int pct = power.getBatteryPercent();
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int fill = (x1 - x0) * pct / 100;
  if (fill > 0) cv->fillRoundRect(x0, y, fill, h, 3, COL_FILL);
}

static int row_at(int16_t y, int rows)
{
  if (rows <= 0) return -1;
  int stride = ROW_H + 8;
  int top0 = ROW_Y0;
  int bot = top0 + rows * stride - 8;
  if (y < top0 || y >= bot) return -1;
  int row = (y - top0) / stride;
  if (row < 0) row = 0;
  if (row >= rows) row = rows - 1;
  return row;
}

static void go_idle(void)
{
  s_mode = MODE_IDLE;
  s_ui_dirty = true;
  s_key_held = -1;
  s_btn_held = -1;
  gfx->setBrightness(200);
}

static void open_admin_gate(void)
{
  s_mode = MODE_ADMIN_GATE;
  s_ui_dirty = true;
  s_key_held = -1;
  s_btn_held = -1;
  gfx->setBrightness(255);
}

static void open_admin_pin(void)
{
  s_pin_len = 0; s_pin[0] = 0;
  s_mode = MODE_ADMIN_PIN;
  s_ui_dirty = true;
  s_key_held = -1;
  s_btn_held = -1;
}

static void open_admin_menu(void)
{
  s_mode = MODE_ADMIN_MENU;
  s_ui_dirty = true;
  s_key_held = -1;
  s_btn_held = -1;
  USBSerial.println("admin: unlocked");
}

static void pin_submit(void)
{
  if (strcmp(s_pin, ADMIN_CODE) == 0) open_admin_menu();
  else {
    s_pin_len = 0; s_pin[0] = 0; s_ui_dirty = true;
    USBSerial.println("admin: bad PIN");
  }
}

static void pin_append(char c)
{
  if (s_pin_len >= 5) return;
  s_pin[s_pin_len++] = c; s_pin[s_pin_len] = 0; s_ui_dirty = true;
  if (s_pin_len == 5) pin_submit();
}

static void pin_del(void)
{
  if (!s_pin_len) return;
  s_pin[--s_pin_len] = 0; s_ui_dirty = true;
}

static int8_t pin_key_at(int16_t x, int16_t y)
{
  const int stride_x = KEY_W + KEY_GAP, stride_y = KEY_H + KEY_GAP;
  const int left = KEY_X0 - 12, right = KEY_X0 + 3 * stride_x - KEY_GAP + 12;
  const int top = KEY_Y0 - 10, bot = KEY_Y0 + 4 * stride_y - KEY_GAP + 28;
  if (x < left || x >= right || y < top || y >= bot) return -1;
  int best = -1, best_d = 0x7fffffff;
  for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) {
    int cx = KEY_X0 + c * stride_x + KEY_W / 2;
    int cy = KEY_Y0 + r * stride_y + KEY_H / 2;
    if (r == 3) cy -= 8;
    int dx = x - cx, dy = y - cy, d = dx * dx + dy * dy;
    if (d < best_d) { best_d = d; best = r * 3 + c; }
  }
  return (int8_t)best;
}

static void pin_fire_key(int8_t idx)
{
  if (idx < 0 || idx > 11) return;
  const char *k = PIN_KEYS[idx / 3][idx % 3];
  if (k[0] == '<') pin_del();
  else if (k[0] == 'O') pin_submit();
  else pin_append(k[0]);
}

static void draw_admin_gate(void)
{
  cv->fillScreen(0);
  cv->drawRect(4, 4, SCR_W - 8, SCR_H - 8, COL_ADMIN);
  cv->setTextSize(2); cv->setTextColor(COL_ADMIN); cv->setCursor(90, 40); cv->print("ADMIN");
  cv->setTextSize(1); cv->setTextColor(COL_MUTE); cv->setCursor(40, 58); cv->print("instructor unlock");
  draw_btn(ROW_Y0, "ADMIN", s_btn_held == 0 ? COL_ADMIN : COL_ADMIN_DK, COL_ADMIN);
  draw_btn(ROW_Y0 + (ROW_H + 8), "CANCEL", s_btn_held == 1 ? COL_ADMIN : 0x3186, COL_MUTE);
  draw_battery_line();
}

static void draw_admin_pin(void)
{
  cv->fillScreen(0);
  cv->drawRect(4, 4, SCR_W - 8, SCR_H - 8, COL_ADMIN);
  cv->setTextSize(2); cv->setTextColor(COL_ADMIN); cv->setCursor(40, 14); cv->print("ENTER PIN");
  cv->fillRoundRect(20, 56, SCR_W - 40, 34, 8, 0x2104);
  cv->setTextSize(2); cv->setTextColor(COL_INK);
  char shown[8];
  for (uint8_t i = 0; i < 5; i++) shown[i] = (i < s_pin_len) ? s_pin[i] : '_';
  shown[5] = 0;
  cv->setCursor(100, 64); cv->print(shown);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) {
    int idx = r * 3 + c;
    int x = KEY_X0 + c * (KEY_W + KEY_GAP);
    int y = KEY_Y0 + r * (KEY_H + KEY_GAP);
    const char *k = PIN_KEYS[r][c];
    uint16_t bg = (idx == s_key_held) ? COL_ADMIN : (k[0] == 'O') ? COL_OK_BG : COL_ADMIN_DK;
    cv->fillRoundRect(x, y, KEY_W, KEY_H, 8, bg);
    cv->drawRoundRect(x, y, KEY_W, KEY_H, 8, COL_ADMIN);
    cv->setTextSize(2); cv->setTextColor(COL_INK);
    int tw = (int)strlen(k) * 12;
    cv->setCursor(x + (KEY_W - tw) / 2, y + 18); cv->print(k);
  }
}

static void draw_admin_menu(void)
{
  cv->fillScreen(0);
  cv->drawRect(4, 4, SCR_W - 8, SCR_H - 8, COL_ADMIN);
  cv->setTextSize(2); cv->setTextColor(COL_ADMIN); cv->setCursor(70, 24); cv->print("ADMIN");
  cv->setTextSize(1); cv->setTextColor(COL_MUTE); cv->setCursor(24, 54);
  cv->printf("active: %s", GAME->title);

  char g1[24], g2[24];
  snprintf(g1, sizeof g1, "GAME 1%s", s_game == 0 ? " *" : "");
  snprintf(g2, sizeof g2, "GAME 2%s", s_game == 1 ? " *" : "");

  draw_btn(ROW_Y0, "RESET BOBBERS",
           s_btn_held == 0 ? COL_AMBER : COL_AMBER_DK, COL_AMBER);
  draw_btn(ROW_Y0 + (ROW_H + 8), g1,
           s_btn_held == 1 ? COL_ADMIN : (s_game == 0 ? COL_ADMIN_DK : 0x2104), COL_ADMIN);
  draw_btn(ROW_Y0 + 2 * (ROW_H + 8), g2,
           s_btn_held == 2 ? COL_ADMIN : (s_game == 1 ? COL_ADMIN_DK : 0x2104), COL_ADMIN);
  draw_btn(ROW_Y0 + 3 * (ROW_H + 8), "DONE",
           s_btn_held == 3 ? COL_ADMIN : 0x2104, COL_MUTE);
  draw_battery_line();
}

static void fire_admin_btn(int8_t row)
{
  if (s_mode == MODE_ADMIN_GATE) {
    if (row == 0) open_admin_pin();
    else if (row == 1) go_idle();
  } else if (s_mode == MODE_ADMIN_MENU) {
    if (row == 0) {
      memset(s_bobbers, 0, sizeof s_bobbers);
      s_ui_dirty = true;
      USBSerial.println("admin: bobbers cleared");
    } else if (row == 1) {
      apply_game(0, true);
      s_ui_dirty = true;
    } else if (row == 2) {
      apply_game(1, true);
      s_ui_dirty = true;
    } else if (row == 3) {
      go_idle();
    }
  }
}

static void render_idle(uint32_t now)
{
  if ((int32_t)(now - s_react_until) >= 0) { s_angry = false; s_happy = false; }
  float t = now / 1000.0f;
  int bob = (int)lroundf(sinf(t * 2.2f) * 4.0f);
  if (s_happy) bob = (int)lroundf(sinf(now / 80.0f) * 6.0f);
  bool hot = s_happy;
  bool blink = (now % BLINK_PERIOD_MS) < BLINK_MS;
  cv->fillScreen(COL_SKY);
  draw_rings(now); draw_water(t); draw_reflection(bob); draw_crests();
  for (int i = 0; i < 2; i++) {
    float ph = fmodf(t * RIPPLE_RATE + i / 2.0f, 1.0f);
    draw_ellipse(DUCK_CX, WATER_Y + 6, 40 + (int)(ph * 120), 4 + (int)(ph * 8), COL_RIPPLE);
  }
  draw_duck(bob, hot, blink);
  draw_tally(now);
  draw_battery_line();
  cv->flush();
}

static void render_frame(uint32_t now)
{
  if (s_mode == MODE_ADMIN_GATE || s_mode == MODE_ADMIN_PIN || s_mode == MODE_ADMIN_MENU) {
    /* Refresh admin periodically so the battery meter stays live. */
    static uint32_t s_batt_tick = 0;
    if ((int32_t)(now - s_batt_tick) >= 2000) {
      s_batt_tick = now;
      s_ui_dirty = true;
    }
    if (!s_ui_dirty) return;
    if (s_mode == MODE_ADMIN_GATE) draw_admin_gate();
    else if (s_mode == MODE_ADMIN_PIN) draw_admin_pin();
    else draw_admin_menu();
    cv->flush();
    s_ui_dirty = false;
    return;
  }
  render_idle(now);
}

static void read_xy(int16_t *x, int16_t *y)
{
  *x = (int16_t)CST816->IIC_Read_Device_Value(
    CST816->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
  *y = (int16_t)CST816->IIC_Read_Device_Value(
    CST816->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
}

static String read_gesture(void)
{
  return CST816->IIC_Read_Device_State(
    CST816->Arduino_IIC_Touch::Status_Information::TOUCH_GESTURE_ID);
}

static void poll_touch(uint32_t now)
{
  if (!CST816) return;
  int fingers = (int)CST816->IIC_Read_Device_Value(
    CST816->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);

  bool overlay = (s_mode != MODE_IDLE);
  if (overlay) {
    if (CST816->IIC_Interrupt_Flag) CST816->IIC_Interrupt_Flag = false;
    if (fingers > 0) {
      int16_t x, y; read_xy(&x, &y);
      if (!s_down) {
        s_down = true; s_tdown = now; s_tx0 = s_tx = x; s_ty0 = s_ty = y;
        if (s_mode == MODE_ADMIN_PIN) {
          s_key_held = pin_key_at(x, y); s_ui_dirty = true;
        } else {
          s_btn_held = (int8_t)row_at(y, s_mode == MODE_ADMIN_GATE ? 2 : 4);
          if (s_btn_held >= 0) s_ui_dirty = true;
        }
      } else { s_tx = x; s_ty = y; }
    } else if (s_down) {
      uint32_t dt = now - s_tdown;
      int dx = abs(s_tx - s_tx0), dy = abs(s_ty - s_ty0);
      s_down = false;
      if (s_mode == MODE_ADMIN_PIN) {
        int8_t key = s_key_held; s_key_held = -1; s_ui_dirty = true;
        if (key >= 0 && dx < 55 && dy < 55 && dt < 1200) pin_fire_key(key);
      } else {
        int8_t btn = s_btn_held; s_btn_held = -1; s_ui_dirty = true;
        if (btn >= 0 && dx < 80 && dy < 80 && dt < 2000 && dt > 15)
          fire_admin_btn(btn);
      }
    }
    return;
  }

  if (CST816->IIC_Interrupt_Flag) {
    CST816->IIC_Interrupt_Flag = false;
    String g = read_gesture();
    if (g == "Swipe Left" && now - s_gest_cool > 400) {
      s_gest_cool = now;
      open_admin_gate();
      return;
    }
  }

  if (fingers > 0) {
    int16_t x, y; read_xy(&x, &y);
    if (!s_down) {
      s_down = true; s_tdown = now; s_tx0 = s_tx = x; s_ty0 = s_ty = y;
    } else { s_tx = x; s_ty = y; }
  } else if (s_down) {
    int dx = s_tx - s_tx0, dy = abs(s_ty - s_ty0);
    uint32_t dt = now - s_tdown;
    s_down = false;
    if (dx < -SWIPE_MIN_DX && dy < SWIPE_MAX_CROSS && dt < 800 && now - s_gest_cool > 400) {
      s_gest_cool = now;
      open_admin_gate();
    }
  }
}

static void poll_buttons(uint32_t now)
{
  if (s_mode != MODE_IDLE) return;
  bool boot = digitalRead(BOOT_PIN) == LOW;
  if (boot != s_boot_down && (now - s_boot_change) > BTN_DEBOUNCE_MS) {
    s_boot_down = boot; s_boot_change = now;
    if (boot && (now - s_btn_cool) > BTN_COOL_MS) { s_btn_cool = now; cycle_tier(+1); }
  }
  if (!s_expander_ok) return;
  bool pwr = expander.digitalRead(PWR_EXIO) == HIGH;
  if (pwr != s_pwr_down && (now - s_pwr_change) > BTN_DEBOUNCE_MS) {
    s_pwr_down = pwr; s_pwr_change = now;
    if (pwr && (now - s_btn_cool) > BTN_COOL_MS) { s_btn_cool = now; cycle_tier(-1); }
  }
}

void setup()
{
  USBSerial.begin(115200);
  USBSerial.setTxTimeoutMs(0);
  USBSerial.println("WiCyS Game — BLE duck challenges");

  pinMode(BOOT_PIN, INPUT_PULLUP);
  Wire.begin(IIC_SDA, IIC_SCL);

  if (power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    s_pmu_ok = true;
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableSystemVoltageMeasure();
    power.enableVbusVoltageMeasure();
    USBSerial.printf("AXP2101 OK  batt=%s %d%%\n",
                     power.isBatteryConnect() ? "yes" : "no",
                     power.isBatteryConnect() ? power.getBatteryPercent() : -1);
  } else {
    USBSerial.println("AXP2101 not found — battery meter disabled");
  }

  if (expander.begin(0x20)) {
    s_expander_ok = true;
    const int pwr_rails[] = {0, 1, 2, 6};
    for (int i = 0; i < 4; i++) { expander.pinMode(pwr_rails[i], OUTPUT); expander.digitalWrite(pwr_rails[i], LOW); }
    delay(20);
    for (int i = 0; i < 4; i++) expander.digitalWrite(pwr_rails[i], HIGH);
    expander.pinMode(PWR_EXIO, INPUT);
  }

  for (int i = 0; i < 5 && !CST816->begin(); i++) delay(400);
  if (CST816->begin()) {
    CST816->IIC_Write_Device_State(
      CST816->Arduino_IIC_Touch::Device::TOUCH_DEVICE_INTERRUPT_MODE,
      CST816->Arduino_IIC_Touch::Device_Mode::TOUCH_DEVICE_INTERRUPT_PERIODIC);
  }

  cv = new Arduino_Canvas(SCR_W, SCR_H, gfx);
  if (!cv->begin()) { USBSerial.println("canvas fail"); while (1) delay(1000); }
  gfx->setBrightness(200);

  s_prefs.begin("wicys", false);
  s_tier = s_prefs.getUChar("tier", 0);
  if (s_tier >= N_TIERS) s_tier = 0;
  s_game = s_prefs.getUChar("game", 0);
  if (s_game >= N_GAMES) s_game = 0;
  apply_tier_palette();
  ble_init();
  s_next_scan = millis() + 2000;

  USBSerial.println("swipe LEFT=admin PIN 24650 | BOOT/PWR=colour");
  USBSerial.printf("loaded %s\n", GAME->title);
}

void loop()
{
  uint32_t now = millis();
  poll_buttons(now);
  poll_touch(now);
  poll_game_sync(now);

  if (USBSerial.available()) {
    char c = USBSerial.read();
    if (c == 'n' || c == 'N') cycle_tier(+1);
    if (c == 'p' || c == 'P') cycle_tier(-1);
    if (c == '1') apply_game(0, true);
    if (c == '2') apply_game(1, true);
    if (c == 'a' || c == 'A') open_admin_gate();
    if (c == 'r' || c == 'R') {
      reset_progress();
      ble_publish();
      USBSerial.println("progress reset");
    }
  }

  static uint32_t s_last = 0;
  if (now - s_last >= FRAME_MS) {
    s_last = now;
    render_frame(now);
  }
}
