/*
 * WiCyS Game — CYD port (ESP32-2432S028 / TPM408-2.8 ILI9341)
 *
 * Phone: nRF Connect → DUCK-<COLOUR> → Read STORY → Write SUBMIT (Text) → Read FLAG
 * BOOT short = next colour · BOOT hold 2s = admin · left-edge tap = previous colour
 * Admin PIN 24650: reset progress/bobbers, Game 1 / Game 2
 * Screen: duck only — scenario lives in STORY
 */
#include <Arduino.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLEAdvertising.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include "pin_config.h"
#include "duck_identity.h"
#include "lgfx_cyd.hpp"

static LGFX_CYD hw;              /* physical panel */
static LGFX_Sprite spr(&hw);     /* off-screen frame */
static lgfx::LGFXBase *g_draw = &hw;
static bool s_sprite_ok = false;
#define lcd (*g_draw)            /* all scene draws go through g_draw */
SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

/* 16-bit half-height bands — full 16-bit FB won't fit with BLE (~150KB). */
#define BAND_H (SCR_H / 2)
static int s_y_bias = 0;

static inline void dPixel(int x, int y, uint16_t c)
{
  y -= s_y_bias;
  if ((unsigned)x < (unsigned)SCR_W && (unsigned)y < (unsigned)BAND_H) lcd.drawPixel(x, y, c);
}
static inline void dFillRect(int x, int y, int w, int h, uint16_t c)
{
  y -= s_y_bias;
  if (w <= 0 || h <= 0) return;
  if (y >= BAND_H || (y + h) <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if ((y + h) > BAND_H) h = BAND_H - y;
  if (x < 0) { w += x; x = 0; }
  if ((x + w) > SCR_W) w = SCR_W - x;
  if (w > 0 && h > 0) lcd.fillRect(x, y, w, h, c);
}
static inline void dFillRoundRect(int x, int y, int w, int h, int r, uint16_t c)
{
  y -= s_y_bias;
  if (y + h <= 0 || y >= BAND_H) return;
  lcd.fillRoundRect(x, y, w, h, r, c);
}
static inline void dDrawRoundRect(int x, int y, int w, int h, int r, uint16_t c)
{
  y -= s_y_bias;
  if (y + h <= 0 || y >= BAND_H) return;
  lcd.drawRoundRect(x, y, w, h, r, c);
}
static inline void dDrawRect(int x, int y, int w, int h, uint16_t c)
{
  y -= s_y_bias;
  lcd.drawRect(x, y, w, h, c);
}
static inline void dCircle(int x, int y, int r, uint16_t c)
{
  y -= s_y_bias;
  if (y + r < 0 || y - r >= BAND_H) return;
  lcd.drawCircle(x, y, r, c);
}
static inline void dFillCircle(int x, int y, int r, uint16_t c)
{
  y -= s_y_bias;
  if (y + r < 0 || y - r >= BAND_H) return;
  lcd.fillCircle(x, y, r, c);
}
static inline void dVLine(int x, int y, int h, uint16_t c)
{
  y -= s_y_bias;
  if (h <= 0) return;
  if (y >= BAND_H || y + h <= 0) return;
  if (y < 0) { h += y; y = 0; }
  if (y + h > BAND_H) h = BAND_H - y;
  if (h > 0) lcd.drawFastVLine(x, y, h, c);
}
static inline void dCursor(int x, int y)
{
  lcd.setCursor(x, y - s_y_bias);
}

static inline void present_band(void)
{
  if (s_sprite_ok) spr.pushSprite(0, s_y_bias);
  else {
    /* direct-to-panel path already drew this band into hw via lcd */
  }
}

#define TARGET_FPS 15
#define FRAME_MS   (1000 / TARGET_FPS)

#define COL_SKY        0x0000
#define COL_WATER      0x09CB
#define COL_DEEP       0x0947
#define COL_CREST      0x3C99
#define COL_RIPPLE     0x8EBF
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
#define SPR_CELL 14
#define DUCK_OX  ((SCR_W - SPR_COLS * SPR_CELL) / 2)
#define DUCK_OY  28
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
#define RING_R0           20
#define RING_R1          140
#define RING_LEVEL      0.38f
#define BLINK_PERIOD_MS 10000
#define BLINK_MS          180
#define RIPPLE_RATE     0.6f
#define WATER_Y          220
#define WATER_DEPTH_Y    270
#define WAVE_COL_W         4
#define WAVE_A1          2.6f
#define WAVE_A2          1.4f
#define BOB_R             14
#define TALLY_MAX          6
#define TALLY_X0          18
#define TALLY_DX          34

#define BTN_DEBOUNCE_MS  35
#define BTN_COOL_MS     280
#define BOOT_ADMIN_MS  2000
#define REACT_MS        900
#define WRONG_COOL_MS   800
#define ADMIN_CODE       "24650"
#define SWIPE_MIN_DX      50
#define SWIPE_MAX_CROSS   70
#define SYNC_BROADCAST_MS 20000
#define SCAN_EVERY_MS     4000

#define BLE_SVC_UUID    "d0c40010-0000-1000-8000-00805f9b34fb"
#define BLE_STORY_UUID  "d0c40011-0000-1000-8000-00805f9b34fb"
#define BLE_SUBMIT_UUID "d0c40012-0000-1000-8000-00805f9b34fb"
#define BLE_FLAG_UUID   "d0c40013-0000-1000-8000-00805f9b34fb"
#define BLE_STATUS_UUID "d0c40014-0000-1000-8000-00805f9b34fb"

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
  const char *colour_need;
  const char *done;
} game_def_t;

/* Answers/flags are unique per DUCK_SERIAL (see duck_identity.h). */
static const game_def_t GAMES_BY_DUCK[DUCK_SERIAL_MAX][N_GAMES] = {
  { /* Duck 1 */
    {
      "Game 1",
      {
        "Hi! I am Duck-1. Write QUACK to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: UE9ORA== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write HOME to SUBMIT as Text."
      },
      { "QUACK", "POND", "HOME" },
      { "WiCyS{d01_g1_quack}", "WiCyS{d01_g1_pond}", "WiCyS{d01_g1_home}" },
      "yellow",
      "You helped Duck-1 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-1: write WADDLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: QlJFQUQ= (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write NEST to SUBMIT as Text."
      },
      { "WADDLE", "BREAD", "NEST" },
      { "WiCyS{d01_g2_waddle}", "WiCyS{d01_g2_bread}", "WiCyS{d01_g2_nest}" },
      "green",
      "Round 2 complete for Duck-1. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 2 */
    {
      "Game 1",
      {
        "Hi! I am Duck-2. Write HONK to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: TEFLRQ== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write NEST to SUBMIT as Text."
      },
      { "HONK", "LAKE", "NEST" },
      { "WiCyS{d02_g1_honk}", "WiCyS{d02_g1_lake}", "WiCyS{d02_g1_nest}" },
      "yellow",
      "You helped Duck-2 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-2: write PADDLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: Q1JVTUI= (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write PERCH to SUBMIT as Text."
      },
      { "PADDLE", "CRUMB", "PERCH" },
      { "WiCyS{d02_g2_paddle}", "WiCyS{d02_g2_crumb}", "WiCyS{d02_g2_perch}" },
      "green",
      "Round 2 complete for Duck-2. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 3 */
    {
      "Game 1",
      {
        "Hi! I am Duck-3. Write PEEP to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: UE9PTA== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write BARN to SUBMIT as Text."
      },
      { "PEEP", "POOL", "BARN" },
      { "WiCyS{d03_g1_peep}", "WiCyS{d03_g1_pool}", "WiCyS{d03_g1_barn}" },
      "yellow",
      "You helped Duck-3 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-3: write DABBLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: U0VFRFM= (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write BRANCH to SUBMIT as Text."
      },
      { "DABBLE", "SEEDS", "BRANCH" },
      { "WiCyS{d03_g2_dabble}", "WiCyS{d03_g2_seeds}", "WiCyS{d03_g2_branch}" },
      "green",
      "Round 2 complete for Duck-3. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 4 */
    {
      "Game 1",
      {
        "Hi! I am Duck-4. Write CHIRP to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: UkVFRA== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write COOP to SUBMIT as Text."
      },
      { "CHIRP", "REED", "COOP" },
      { "WiCyS{d04_g1_chirp}", "WiCyS{d04_g1_reed}", "WiCyS{d04_g1_coop}" },
      "yellow",
      "You helped Duck-4 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-4: write BOBBLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: Q09STg== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write LEDGE to SUBMIT as Text."
      },
      { "BOBBLE", "CORN", "LEDGE" },
      { "WiCyS{d04_g2_bobble}", "WiCyS{d04_g2_corn}", "WiCyS{d04_g2_ledge}" },
      "green",
      "Round 2 complete for Duck-4. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 5 */
    {
      "Game 1",
      {
        "Hi! I am Duck-5. Write SQUAWK to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: RE9DSw== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write YARD to SUBMIT as Text."
      },
      { "SQUAWK", "DOCK", "YARD" },
      { "WiCyS{d05_g1_squawk}", "WiCyS{d05_g1_dock}", "WiCyS{d05_g1_yard}" },
      "yellow",
      "You helped Duck-5 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-5: write TODDLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: R1JBSU4= (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write TWIG to SUBMIT as Text."
      },
      { "TODDLE", "GRAIN", "TWIG" },
      { "WiCyS{d05_g2_toddle}", "WiCyS{d05_g2_grain}", "WiCyS{d05_g2_twig}" },
      "green",
      "Round 2 complete for Duck-5. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 6 */
    {
      "Game 1",
      {
        "Hi! I am Duck-6. Write FLAP to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: V0FWRQ== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write PATH to SUBMIT as Text."
      },
      { "FLAP", "WAVE", "PATH" },
      { "WiCyS{d06_g1_flap}", "WiCyS{d06_g1_wave}", "WiCyS{d06_g1_path}" },
      "yellow",
      "You helped Duck-6 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-6: write AMBLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: V0hFQVQ= (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write LIMB to SUBMIT as Text."
      },
      { "AMBLE", "WHEAT", "LIMB" },
      { "WiCyS{d06_g2_amble}", "WiCyS{d06_g2_wheat}", "WiCyS{d06_g2_limb}" },
      "green",
      "Round 2 complete for Duck-6. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 7 */
    {
      "Game 1",
      {
        "Hi! I am Duck-7. Write DIVE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: UkFJTg== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write GATE to SUBMIT as Text."
      },
      { "DIVE", "RAIN", "GATE" },
      { "WiCyS{d07_g1_dive}", "WiCyS{d07_g1_rain}", "WiCyS{d07_g1_gate}" },
      "yellow",
      "You helped Duck-7 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-7: write SHUFFLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: T0FUUw== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write BOUGH to SUBMIT as Text."
      },
      { "SHUFFLE", "OATS", "BOUGH" },
      { "WiCyS{d07_g2_shuffle}", "WiCyS{d07_g2_oats}", "WiCyS{d07_g2_bough}" },
      "green",
      "Round 2 complete for Duck-7. Your FLAG is still in FLAG."
    }
  },
  { /* Duck 8 */
    {
      "Game 1",
      {
        "Hi! I am Duck-8. Write SWIM to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Map scrambled: U0hJUA== (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only YELLOW feathers trust me. Use BOOT / left edge, then write ROOST to SUBMIT as Text."
      },
      { "SWIM", "SHIP", "ROOST" },
      { "WiCyS{d08_g1_swim}", "WiCyS{d08_g1_ship}", "WiCyS{d08_g1_roost}" },
      "yellow",
      "You helped Duck-8 get home. Nice work! Your FLAG is still in FLAG."
    },
    {
      "Game 2",
      {
        "Round 2! Duck-8: write SCUFFLE to SUBMIT as Text (not Hex). Then Read FLAG.",
        "Snack code: TUFJWkU= (Base64). Decode to one word. Write it to SUBMIT as Text.",
        "Only GREEN feathers trust me. Use BOOT / left edge, then write STICK to SUBMIT as Text."
      },
      { "SCUFFLE", "MAIZE", "STICK" },
      { "WiCyS{d08_g2_scuffle}", "WiCyS{d08_g2_maize}", "WiCyS{d08_g2_stick}" },
      "green",
      "Round 2 complete for Duck-8. Your FLAG is still in FLAG."
    }
  }
};

#define GAME (&GAMES_BY_DUCK[DUCK_IX][s_game])

typedef enum {
  MODE_IDLE, MODE_ADMIN_GATE, MODE_ADMIN_PIN, MODE_ADMIN_MENU, MODE_TOUCH_CAL,
} ui_mode_t;

#define ROW_H  40
#define ROW_X  12
#define ROW_W  (SCR_W - 24)
#define ROW_Y0 48
#define KEY_W 68
#define KEY_H 42
#define KEY_X0 14
#define KEY_Y0 88
#define KEY_GAP 4

static const char *PIN_KEYS[4][3] = {
  { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" }, { "<", "0", "OK" },
};

static int16_t  s_surf[SCR_W / WAVE_COL_W];
static char     s_duck_name[28] = "Yellow-Duck-1";
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

/* Touch cal: map raw ADC → framebuffer (survives reboot via NVS). */
static bool    s_cal_ok = false;
static bool    s_cal_swap = true;   /* CYD default: raw axes swapped */
static bool    s_cal_flip_x = false;
static bool    s_cal_flip_y = false;
static int16_t s_cal_rx0 = TOUCH_X_MIN, s_cal_rx1 = TOUCH_X_MAX;
static int16_t s_cal_ry0 = TOUCH_Y_MIN, s_cal_ry1 = TOUCH_Y_MAX;
static uint8_t s_cal_step = 0;
static int16_t s_cal_raw[3][2];
static bool    s_cal_have_raw = false;

static BLECharacteristic *s_ch_story  = nullptr;
static BLECharacteristic *s_ch_flag   = nullptr;
static BLECharacteristic *s_ch_status = nullptr;
static BLEScan *s_scan = nullptr;

static bool     s_boot_down = false;
static bool     s_boot_long_fired = false;
static uint32_t s_boot_change = 0;
static uint32_t s_boot_down_at = 0;
static uint32_t s_btn_cool    = 0;

static void apply_game(uint8_t g, bool broadcast);
static void ble_restart_adv(void);
static void ble_publish(void);
static void open_touch_cal(void);
static void open_admin_gate(void);
static void open_admin_pin(void);
static void go_idle(void);

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
    PAL_REFL_BODY[i] = blend565(COL_WATER, PAL_CALM[i], 72);
    PAL_REFL_DEEP[i] = blend565(COL_DEEP,  PAL_CALM[i], 72);
  }
}

static void build_duck_name(void)
{
  /* e.g. Yellow-Duck-1 — colour title-case + fixed serial */
  char colour[12];
  snprintf(colour, sizeof colour, "%s", TIER->name);
  if (colour[0]) colour[0] = (char)toupper((unsigned char)colour[0]);
  for (char *p = colour + 1; *p; ++p) *p = (char)tolower((unsigned char)*p);
  snprintf(s_duck_name, sizeof s_duck_name, "%s-Duck-%u", colour, (unsigned)DUCK_SERIAL);
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
    Serial.printf("admin: broadcast %s to flock\n", GAME->title);
  }
  ble_restart_adv();
  Serial.printf("game -> %s\n", GAME->title);
}

static void normalize_answer(char *dst, size_t dst_sz, const uint8_t *data, size_t len)
{
  size_t o = 0;
  size_t i = 0;
  if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) i = 3;

  /* UTF-16LE text from some phones: Q\0 U\0 A\0 … */
  bool utf16le = (len - i) >= 4 && data[i + 1] == 0x00 && data[i + 3] == 0x00;

  while (i < len && o + 1 < dst_sz) {
    unsigned char c;
    if (utf16le) {
      if (i + 1 >= len) break;
      c = data[i];
      i += 2;
    } else if (i + 2 < len && data[i] == 0xEF &&
               (data[i + 1] == 0xBC || data[i + 1] == 0xBD)) {
      /* Fullwidth Latin (common when phones "helpfully" prettify ALL CAPS) */
      unsigned char b2 = data[i + 1], b3 = data[i + 2];
      i += 3;
      if (b2 == 0xBC && b3 >= 0xA1 && b3 <= 0xBA) c = (unsigned char)('A' + (b3 - 0xA1));
      else if (b2 == 0xBD && b3 >= 0x81 && b3 <= 0x9A) c = (unsigned char)('a' + (b3 - 0x81));
      else if (b2 == 0xBC && b3 >= 0x90 && b3 <= 0x99) c = (unsigned char)('0' + (b3 - 0x90));
      else continue;
    } else {
      c = data[i++];
    }
    if (c == 0) continue;
    if (!isalnum(c)) continue;
    dst[o++] = (char)toupper(c);
  }
  dst[o] = 0;
}

static void on_submit(const uint8_t *data, size_t len)
{
  uint32_t now = millis();
  if ((int32_t)(now - s_wrong_cool) < 0) return;

  char ans[40];
  normalize_answer(ans, sizeof ans, data, len);
  Serial.printf("submit: '%s' duck=%u game=%u stage=%u colour=%s raw=",
                ans, (unsigned)DUCK_SERIAL, (unsigned)s_game,
                (unsigned)s_stage, TIER->name);
  for (size_t i = 0; i < len && i < 24; i++) Serial.printf("%02X", data[i]);
  if (len > 24) Serial.print("...");
  Serial.println();

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
  Serial.printf("stage %u solved -> %s\n", (unsigned)s_stage, s_flag_buf);

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
  if (!s_ble_ok) return;
  const char *story = (s_stage < N_STAGES) ? GAME->story[s_stage] : GAME->done;
  s_ch_story->setValue(story);
  s_ch_flag->setValue(s_flag_buf);
  s_ch_status->setValue(s_status);
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
  Serial.printf("BLE advertising as %s (%s)%s\n",
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
  Serial.printf("BLE MAC %s | %s\n", mac.c_str(), GAME->title);
}

static void poll_game_sync(uint32_t now)
{
  if (!s_ble_ok || !s_scan) return;

  if (s_pending_game >= 0) {
    uint8_t g = (uint8_t)s_pending_game;
    s_pending_game = -1;
    Serial.printf("sync: flock set -> %s\n", GAMES_BY_DUCK[DUCK_IX][g].title);
    apply_game(g, false);
  }

  static bool was_sync = false;
  bool syncing = (int32_t)(now - s_sync_until) < 0;
  if (was_sync && !syncing) ble_restart_adv();
  was_sync = syncing;

  if (s_mode != MODE_IDLE) return;
  if ((int32_t)(now - s_next_scan) < 0) return;
  s_next_scan = now + SCAN_EVERY_MS;
  s_scan->start(1, false);
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
  for (int i = 0; i < 36; i++) {
    float a = (float)i * 2.0f * (float)PI / 36;
    int x = cx + (int)(rx * cosf(a));
    int y = cy + (int)(ry * sinf(a));
    if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H) dPixel(x, y, colour);
  }
}

static void draw_water(float t)
{
  for (int i = 0; i < SCR_W / WAVE_COL_W; i++) {
    int x = i * WAVE_COL_W;
    int s = WATER_Y + (int)lroundf(WAVE_A1 * sinf(x / 26.0f + t * 1.6f)
                                 + WAVE_A2 * sinf(x / 11.0f - t * 2.3f));
    s_surf[i] = s;
    dFillRect(x, s, WAVE_COL_W, SCR_H - s, COL_WATER);
    int d = s > WATER_DEPTH_Y ? s : WATER_DEPTH_Y;
    if (d < SCR_H) dFillRect(x, d, WAVE_COL_W, SCR_H - d, COL_DEEP);
  }
}

static void draw_crests(void)
{
  for (int i = 0; i < SCR_W / WAVE_COL_W; i++)
    dFillRect(i * WAVE_COL_W, s_surf[i], WAVE_COL_W, 2, COL_CREST);
}

static void draw_reflection(int bob)
{
  /* Only below the waterline — never above the duck. */
  for (int r = 0; r < SPR_ROWS; r++) {
    int y = WATER_Y + (SPR_ROWS - 1 - r) * SPR_CELL + 6 - bob;
    if (y < WATER_Y || y >= SCR_H) continue;
    for (int c = 0; c < SPR_COLS; c++) {
      uint8_t ix = spr_index(DUCK_SPR[r][c]);
      if (ix == PX_NONE) continue;
      uint16_t col = (y >= WATER_DEPTH_Y) ? PAL_REFL_DEEP[ix] : PAL_REFL_BODY[ix];
      dFillRect(DUCK_OX + c * SPR_CELL, y, SPR_CELL, SPR_CELL, col);
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
      dFillRect(DUCK_OX + c * SPR_CELL, DUCK_OY + r * SPR_CELL + bob,
                   SPR_CELL, SPR_CELL, col);
    }
  }
  if (s_angry) {
    int ex = DUCK_OX + 6 * SPR_CELL;
    int ey = DUCK_OY + 2 * SPR_CELL + bob;
    dFillRect(ex - 2, ey - 8, SPR_CELL + 14, 4, pal[PX_K]);
  }
}

static void draw_rings(uint32_t now)
{
  for (int i = 0; i < N_RINGS; i++) {
    float ph = fmodf((float)now / RING_LIFE_MS + (float)i / N_RINGS, 1.0f);
    int r = RING_R0 + (int)(ph * (RING_R1 - RING_R0));
    uint8_t gv = (uint8_t)(63.0f * RING_LEVEL * (1.0f - ph));
    if (gv) dCircle(DUCK_CX, DUCK_CY, r, (uint16_t)(gv << 5));
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
    int cy = surf + b + BOB_R - 4;
    dVLine(x, cy - BOB_R - 5, 5, COL_TALLY_ROD);
    dFillCircle(x, cy, BOB_R, COL_TALLY);
    dFillCircle(x - BOB_R / 3, cy - BOB_R / 2, 1, COL_TALLY_LOW);
  }
}

static void draw_btn(int y, const char *txt, uint16_t bg, uint16_t border)
{
  dFillRoundRect(ROW_X, y, ROW_W, ROW_H, 10, bg);
  dDrawRoundRect(ROW_X, y, ROW_W, ROW_H, 10, border);
  lcd.setTextSize(2); lcd.setTextColor(COL_INK);
  int tw = (int)strlen(txt) * 12;
  dCursor(ROW_X + (ROW_W - tw) / 2, y + 14);
  lcd.print(txt);
}

static int row_at(int16_t y, int rows)
{
  if (rows <= 0) return -1;
  /* Fat zones: split the lower screen evenly so bad Y cal still picks a row. */
  const int top = 56;
  if (y < top) return -1;
  int band = SCR_H - top;
  int row = ((int)y - top) * rows / band;
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
}

static void open_admin_gate(void)
{
  /* Without a saved cal, mapped taps all land on ADMIN — go straight to raw cal. */
  if (!s_cal_ok) {
    open_touch_cal();
    return;
  }
  s_mode = MODE_ADMIN_GATE;
  s_ui_dirty = true;
  s_key_held = -1;
  s_btn_held = -1;
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
  Serial.println("admin: unlocked");
}

static void pin_submit(void)
{
  if (strcmp(s_pin, ADMIN_CODE) == 0) open_admin_menu();
  else {
    s_pin_len = 0; s_pin[0] = 0; s_ui_dirty = true;
    Serial.println("admin: bad PIN");
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
  if (x < KEY_X0 || y < KEY_Y0) return -1;
  int c = (x - KEY_X0) / stride_x;
  int r = (y - KEY_Y0) / stride_y;
  if (c < 0 || c > 2 || r < 0 || r > 3) return -1;
  int kx = KEY_X0 + c * stride_x;
  int ky = KEY_Y0 + r * stride_y;
  if (x >= kx + KEY_W || y >= ky + KEY_H) return -1;
  return (int8_t)(r * 3 + c);
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
  lcd.fillScreen(0);
  dDrawRect(2, 2, SCR_W - 4, SCR_H - 4, COL_ADMIN);
  lcd.setTextSize(2); lcd.setTextColor(COL_ADMIN); dCursor(70, 20); lcd.print("ADMIN");
  lcd.setTextSize(1); lcd.setTextColor(COL_MUTE); dCursor(16, 42);
  lcd.print("PIN 24650  |  serial digits OK");
  draw_btn(ROW_Y0, "ADMIN", s_btn_held == 0 ? COL_ADMIN : COL_ADMIN_DK, COL_ADMIN);
  draw_btn(ROW_Y0 + (ROW_H + 6), "CALIBRATE TOUCH",
           s_btn_held == 1 ? COL_AMBER : COL_AMBER_DK, COL_AMBER);
  draw_btn(ROW_Y0 + 2 * (ROW_H + 6), "CANCEL", s_btn_held == 2 ? COL_ADMIN : 0x3186, COL_MUTE);
}

static void draw_admin_pin(void)
{
  lcd.fillScreen(0);
  dDrawRect(2, 2, SCR_W - 4, SCR_H - 4, COL_ADMIN);
  lcd.setTextSize(2); lcd.setTextColor(COL_ADMIN); dCursor(40, 10); lcd.print("ENTER PIN");
  dFillRoundRect(16, 40, SCR_W - 32, 28, 6, 0x2104);
  lcd.setTextSize(2); lcd.setTextColor(COL_INK);
  char shown[8];
  for (uint8_t i = 0; i < 5; i++) shown[i] = (i < s_pin_len) ? s_pin[i] : '_';
  shown[5] = 0;
  dCursor(70, 46); lcd.print(shown);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 3; c++) {
    int idx = r * 3 + c;
    int x = KEY_X0 + c * (KEY_W + KEY_GAP);
    int y = KEY_Y0 + r * (KEY_H + KEY_GAP);
    const char *k = PIN_KEYS[r][c];
    uint16_t bg = (idx == s_key_held) ? COL_ADMIN : (k[0] == 'O') ? COL_OK_BG : COL_ADMIN_DK;
    dFillRoundRect(x, y, KEY_W, KEY_H, 6, bg);
    dDrawRoundRect(x, y, KEY_W, KEY_H, 6, COL_ADMIN);
    lcd.setTextSize(2); lcd.setTextColor(COL_INK);
    int tw = (int)strlen(k) * 12;
    dCursor(x + (KEY_W - tw) / 2, y + 12); lcd.print(k);
  }
}

static void draw_admin_menu(void)
{
  lcd.fillScreen(0);
  dDrawRect(2, 2, SCR_W - 4, SCR_H - 4, COL_ADMIN);
  lcd.setTextSize(2); lcd.setTextColor(COL_ADMIN); dCursor(70, 12); lcd.print("ADMIN");
  lcd.setTextSize(1); lcd.setTextColor(COL_MUTE); dCursor(16, 36);
  lcd.printf("Duck-%u  %s", (unsigned)DUCK_SERIAL, GAME->title);

  char g1[24], g2[24];
  snprintf(g1, sizeof g1, "GAME 1%s", s_game == 0 ? " *" : "");
  snprintf(g2, sizeof g2, "GAME 2%s", s_game == 1 ? " *" : "");

  draw_btn(ROW_Y0, "RESET PROGRESS",
           s_btn_held == 0 ? COL_AMBER : COL_AMBER_DK, COL_AMBER);
  draw_btn(ROW_Y0 + (ROW_H + 6), "RESET BOBBERS",
           s_btn_held == 1 ? COL_AMBER : COL_AMBER_DK, COL_AMBER);
  draw_btn(ROW_Y0 + 2 * (ROW_H + 6), g1,
           s_btn_held == 2 ? COL_ADMIN : (s_game == 0 ? COL_ADMIN_DK : 0x2104), COL_ADMIN);
  draw_btn(ROW_Y0 + 3 * (ROW_H + 6), g2,
           s_btn_held == 3 ? COL_ADMIN : (s_game == 1 ? COL_ADMIN_DK : 0x2104), COL_ADMIN);
  draw_btn(ROW_Y0 + 4 * (ROW_H + 6), "DONE",
           s_btn_held == 4 ? COL_ADMIN : 0x2104, COL_MUTE);
}

static void fire_admin_btn(int8_t row)
{
  if (s_mode == MODE_ADMIN_GATE) {
    if (row == 0) open_admin_pin();
    else if (row == 1) open_touch_cal();
    else if (row == 2) go_idle();
  } else if (s_mode == MODE_ADMIN_MENU) {
    if (row == 0) {
      reset_progress();
      ble_publish();
      s_ui_dirty = true;
      Serial.println("admin: progress reset");
    } else if (row == 1) {
      memset(s_bobbers, 0, sizeof s_bobbers);
      s_ui_dirty = true;
      Serial.println("admin: bobbers cleared");
    } else if (row == 2) {
      apply_game(0, true);
      s_ui_dirty = true;
    } else if (row == 3) {
      apply_game(1, true);
      s_ui_dirty = true;
    } else if (row == 4) {
      go_idle();
    }
  }
}

static void render_idle_band(uint32_t now, int bob, float t, bool hot, bool blink)
{
  lcd.fillScreen(COL_SKY);
  draw_rings(now); draw_water(t); draw_reflection(bob); draw_crests();
  for (int i = 0; i < 2; i++) {
    float ph = fmodf(t * RIPPLE_RATE + i / 2.0f, 1.0f);
    draw_ellipse(DUCK_CX, WATER_Y + 4, 28 + (int)(ph * 90), 3 + (int)(ph * 6), COL_RIPPLE);
  }
  draw_duck(bob, hot, blink);
  draw_tally(now);
}

static void render_idle(uint32_t now)
{
  if ((int32_t)(now - s_react_until) >= 0) { s_angry = false; s_happy = false; }
  float t = now / 1000.0f;
  int bob = (int)lroundf(sinf(t * 2.2f) * 3.0f);
  if (s_happy) bob = (int)lroundf(sinf(now / 80.0f) * 5.0f);
  bool hot = s_happy;
  bool blink = (now % BLINK_PERIOD_MS) < BLINK_MS;
  for (int band = 0; band < 2; band++) {
    s_y_bias = band * BAND_H;
    render_idle_band(now, bob, t, hot, blink);
    present_band();
  }
}

static void render_frame(uint32_t now)
{
  if (s_mode == MODE_ADMIN_GATE || s_mode == MODE_ADMIN_PIN ||
      s_mode == MODE_ADMIN_MENU || s_mode == MODE_TOUCH_CAL) {
    if (!s_ui_dirty) return;
    for (int band = 0; band < 2; band++) {
      s_y_bias = band * BAND_H;
      if (s_mode == MODE_ADMIN_GATE) draw_admin_gate();
      else if (s_mode == MODE_ADMIN_PIN) draw_admin_pin();
      else if (s_mode == MODE_ADMIN_MENU) draw_admin_menu();
      else draw_touch_cal();
      present_band();
    }
    s_ui_dirty = false;
    return;
  }
  render_idle(now);
}

static bool read_touch_raw(int16_t *rx, int16_t *ry, int16_t *rz)
{
  if (!ts.tirqTouched() && !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 200) return false;
  /* setRotation(0): point x/y are the ADC samples */
  *rx = p.x;
  *ry = p.y;
  *rz = p.z;
  return true;
}

static void cal_load(void)
{
  s_cal_ok = s_prefs.getBool("tcal", false);
  if (!s_cal_ok) return;
  s_cal_swap   = s_prefs.getBool("tswap", true);
  s_cal_flip_x = s_prefs.getBool("tfx", false);
  s_cal_flip_y = s_prefs.getBool("tfy", false);
  s_cal_rx0 = (int16_t)s_prefs.getShort("trx0", TOUCH_X_MIN);
  s_cal_rx1 = (int16_t)s_prefs.getShort("trx1", TOUCH_X_MAX);
  s_cal_ry0 = (int16_t)s_prefs.getShort("try0", TOUCH_Y_MIN);
  s_cal_ry1 = (int16_t)s_prefs.getShort("try1", TOUCH_Y_MAX);
  Serial.printf("touch cal loaded swap=%d fx=%d fy=%d rx=%d..%d ry=%d..%d\n",
                (int)s_cal_swap, (int)s_cal_flip_x, (int)s_cal_flip_y,
                (int)s_cal_rx0, (int)s_cal_rx1, (int)s_cal_ry0, (int)s_cal_ry1);
}

static void cal_save(void)
{
  s_prefs.putBool("tcal", true);
  s_prefs.putBool("tswap", s_cal_swap);
  s_prefs.putBool("tfx", s_cal_flip_x);
  s_prefs.putBool("tfy", s_cal_flip_y);
  s_prefs.putShort("trx0", s_cal_rx0);
  s_prefs.putShort("trx1", s_cal_rx1);
  s_prefs.putShort("try0", s_cal_ry0);
  s_prefs.putShort("try1", s_cal_ry1);
  s_cal_ok = true;
  Serial.println("touch cal saved");
}

static bool map_touch(int16_t *x, int16_t *y)
{
  int16_t rx, ry, rz;
  if (!read_touch_raw(&rx, &ry, &rz)) return false;

  int16_t ax = rx, ay = ry;
  if (s_cal_swap) { ax = ry; ay = rx; }

  int16_t x0 = s_cal_rx0, x1 = s_cal_rx1;
  int16_t y0 = s_cal_ry0, y1 = s_cal_ry1;
  if (s_cal_swap) { x0 = s_cal_ry0; x1 = s_cal_ry1; y0 = s_cal_rx0; y1 = s_cal_rx1; }

  int16_t sx = map(ax, x0, x1, 0, SCR_W - 1);
  int16_t sy = map(ay, y0, y1, 0, SCR_H - 1);
  if (s_cal_flip_x) sx = (int16_t)(SCR_W - 1 - sx);
  if (s_cal_flip_y) sy = (int16_t)(SCR_H - 1 - sy);
  if (sx < 0) sx = 0; if (sx >= SCR_W) sx = SCR_W - 1;
  if (sy < 0) sy = 0; if (sy >= SCR_H) sy = SCR_H - 1;
  *x = sx; *y = sy;
  return true;
}

/* Targets in framebuffer space: TL, TR, BL */
static void cal_target(uint8_t step, int16_t *tx, int16_t *ty)
{
  if (step == 0) { *tx = 30;           *ty = 30; }
  else if (step == 1) { *tx = SCR_W - 30; *ty = 30; }
  else { *tx = 30; *ty = SCR_H - 30; }
}

static void open_touch_cal(void)
{
  s_mode = MODE_TOUCH_CAL;
  s_cal_step = 0;
  s_cal_have_raw = false;
  s_ui_dirty = true;
  s_down = false;
  Serial.println("touch cal: tap the crosses");
}

static void cal_finish_from_samples(void)
{
  /* step0=TL, step1=TR, step2=BL — decide swap + extents + flips */
  int16_t (*R)[2] = s_cal_raw;
  int d0x = abs(R[1][0] - R[0][0]);
  int d0y = abs(R[1][1] - R[0][1]);
  s_cal_swap = (d0y > d0x);

  if (s_cal_swap) {
    s_cal_ry0 = R[0][1]; s_cal_ry1 = R[1][1];
    s_cal_rx0 = R[0][0]; s_cal_rx1 = R[2][0];
    s_cal_flip_x = (R[1][1] < R[0][1]);
    s_cal_flip_y = (R[2][0] < R[0][0]);
    if (s_cal_ry0 > s_cal_ry1) { int16_t t=s_cal_ry0; s_cal_ry0=s_cal_ry1; s_cal_ry1=t; }
    if (s_cal_rx0 > s_cal_rx1) { int16_t t=s_cal_rx0; s_cal_rx0=s_cal_rx1; s_cal_rx1=t; }
  } else {
    s_cal_rx0 = R[0][0]; s_cal_rx1 = R[1][0];
    s_cal_ry0 = R[0][1]; s_cal_ry1 = R[2][1];
    s_cal_flip_x = (R[1][0] < R[0][0]);
    s_cal_flip_y = (R[2][1] < R[0][1]);
    if (s_cal_rx0 > s_cal_rx1) { int16_t t=s_cal_rx0; s_cal_rx0=s_cal_rx1; s_cal_rx1=t; }
    if (s_cal_ry0 > s_cal_ry1) { int16_t t=s_cal_ry0; s_cal_ry0=s_cal_ry1; s_cal_ry1=t; }
  }

  cal_save();
  open_admin_gate();
}

static void draw_touch_cal(void)
{
  int16_t tx, ty;
  cal_target(s_cal_step, &tx, &ty);
  lcd.fillScreen(0);
  lcd.setTextSize(2); lcd.setTextColor(COL_ADMIN); dCursor(40, SCR_H / 2 - 40);
  lcd.print("TOUCH CAL");
  lcd.setTextSize(1); lcd.setTextColor(COL_MUTE); dCursor(50, SCR_H / 2 - 10);
  lcd.printf("Tap cross %u / 3", (unsigned)(s_cal_step + 1));
  lcd.setTextColor(COL_AMBER);
  dCursor(20, SCR_H / 2 + 20);
  lcd.print("Use a stylus / fingernail");
  /* crosshair */
  dFillRect(tx - 12, ty - 1, 25, 3, COL_INK);
  dFillRect(tx - 1, ty - 12, 3, 25, COL_INK);
  dFillCircle(tx, ty, 4, COL_AMBER);
}

static void poll_touch(uint32_t now)
{
  if (s_mode == MODE_TOUCH_CAL) {
    int16_t rx, ry, rz;
    bool touching = read_touch_raw(&rx, &ry, &rz);
    if (touching) {
      if (!s_down) {
        s_down = true;
        s_tdown = now;
        s_cal_raw[s_cal_step][0] = rx;
        s_cal_raw[s_cal_step][1] = ry;
        s_cal_have_raw = true;
        Serial.printf("cal[%u] raw=%d,%d\n", (unsigned)s_cal_step, (int)rx, (int)ry);
      }
    } else if (s_down) {
      s_down = false;
      if (s_cal_have_raw && (now - s_tdown) > 40 && (now - s_tdown) < 2000) {
        s_cal_have_raw = false;
        s_cal_step++;
        if (s_cal_step >= 3) cal_finish_from_samples();
        else s_ui_dirty = true;
      } else {
        s_cal_have_raw = false;
      }
    }
    return;
  }

  int16_t x, y;
  bool touching = map_touch(&x, &y);

  bool overlay = (s_mode != MODE_IDLE);
  if (overlay) {
    if (touching) {
      if (!s_down) {
        s_down = true; s_tdown = now; s_tx0 = s_tx = x; s_ty0 = s_ty = y;
        if (s_mode == MODE_ADMIN_PIN) {
          s_key_held = pin_key_at(x, y); s_ui_dirty = true;
          Serial.printf("pin touch (%d,%d) key=%d\n", (int)x, (int)y, (int)s_key_held);
        } else {
          s_btn_held = (int8_t)row_at(y, s_mode == MODE_ADMIN_GATE ? 3 : 5);
          if (s_btn_held >= 0) s_ui_dirty = true;
        }
      } else { s_tx = x; s_ty = y; }
    } else if (s_down) {
      uint32_t dt = now - s_tdown;
      int dx = abs(s_tx - s_tx0), dy = abs(s_ty - s_ty0);
      s_down = false;
      if (s_mode == MODE_ADMIN_PIN) {
        int8_t key = s_key_held; s_key_held = -1; s_ui_dirty = true;
        if (key >= 0 && dx < 40 && dy < 40 && dt < 1200) pin_fire_key(key);
      } else {
        int8_t btn = s_btn_held; s_btn_held = -1; s_ui_dirty = true;
        if (btn >= 0 && dx < 80 && dy < 80 && dt < 2500 && dt > 10)
          fire_admin_btn(btn);
      }
    }
    return;
  }

  if (touching) {
    if (!s_down) {
      s_down = true; s_tdown = now; s_tx0 = s_tx = x; s_ty0 = s_ty = y;
    } else { s_tx = x; s_ty = y; }
  } else if (s_down) {
    int dx = s_tx - s_tx0, dy = abs(s_ty - s_ty0);
    uint32_t dt = now - s_tdown;
    int16_t tap_x = s_tx0;
    s_down = false;
    if (abs(dx) >= SWIPE_MIN_DX && dy < SWIPE_MAX_CROSS && dt < 900 && now - s_gest_cool > 400) {
      s_gest_cool = now;
      open_admin_gate();
      return;
    }
    /* Short tap on left strip → previous colour (no PWR button on CYD). */
    if (dt < 500 && abs(dx) < 25 && dy < 25 && tap_x < 40 &&
        (now - s_btn_cool) > BTN_COOL_MS) {
      s_btn_cool = now;
      cycle_tier(-1);
    }
  }
}

static void poll_buttons(uint32_t now)
{
  if (s_mode != MODE_IDLE) return;
  bool boot = digitalRead(BOOT_PIN) == LOW;
  if (boot != s_boot_down && (now - s_boot_change) > BTN_DEBOUNCE_MS) {
    s_boot_down = boot;
    s_boot_change = now;
    if (boot) {
      s_boot_down_at = now;
      s_boot_long_fired = false;
    } else if (!s_boot_long_fired &&
               (now - s_boot_down_at) < BOOT_ADMIN_MS &&
               (now - s_btn_cool) > BTN_COOL_MS) {
      /* Short press → next colour */
      s_btn_cool = now;
      cycle_tier(+1);
    }
  }
  /* Hold BOOT 2s → admin (CYD has no reliable swipe) */
  if (s_boot_down && !s_boot_long_fired &&
      (now - s_boot_down_at) >= BOOT_ADMIN_MS) {
    s_boot_long_fired = true;
    open_admin_gate();
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.printf("WiCyS Game — CYD serial %u\n", (unsigned)DUCK_SERIAL);

  pinMode(BOOT_PIN, INPUT_PULLUP);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  hw.init();
  hw.setRotation(0);       /* upright — same as preferred TFT_eSPI build */
  hw.setBrightness(255);
  hw.fillScreen(TFT_YELLOW);
  delay(150);
  hw.fillScreen(COL_SKY);
  Serial.printf("panel %dx%d\n", hw.width(), hw.height());

  Serial.printf("heap before sprite: free %u max %u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  /* Two 16-bit bands of 240x160 = 76KB — fits; full frame does not with BLE. */
  spr.setColorDepth(16);
  s_sprite_ok = spr.createSprite(SCR_W, BAND_H);
  if (s_sprite_ok) {
    g_draw = &spr;
    Serial.println("sprite 16-bit banded OK");
  } else {
    g_draw = &hw;
    Serial.println("sprite OOM — drawing direct (may flicker)");
  }

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(0);

  s_prefs.begin("wicys", false);
  cal_load();
  if (s_prefs.getUChar("sid", 0) != (uint8_t)DUCK_SERIAL) {
    s_prefs.putUChar("sid", (uint8_t)DUCK_SERIAL);
    reset_progress();
    s_prefs.putUChar("game", 0);
    Serial.printf("identity -> Duck-%u (progress cleared)\n", (unsigned)DUCK_SERIAL);
  }
  s_tier = s_prefs.getUChar("tier", 0);
  if (s_tier >= N_TIERS) s_tier = 0;
  s_game = s_prefs.getUChar("game", 0);
  if (s_game >= N_GAMES) s_game = 0;
  apply_tier_palette();
  ble_init();
  s_next_scan = millis() + 2000;

  Serial.println("BOOT short=colour | BOOT hold 2s=admin PIN 24650 | left-edge tap=prev | serial R=reset");
  Serial.printf("loaded %s | answers: %s / %s / %s\n",
                GAME->title, GAME->answer[0], GAME->answer[1], GAME->answer[2]);
}

void loop()
{
  uint32_t now = millis();
  poll_buttons(now);
  poll_touch(now);
  poll_game_sync(now);

  if (Serial.available()) {
    char c = Serial.read();
    if (s_mode == MODE_ADMIN_PIN) {
      if (c >= '0' && c <= '9') pin_append(c);
      else if (c == 8 || c == 127 || c == '<') pin_del();
      else if (c == '\r' || c == '\n' || c == 'o' || c == 'O') pin_submit();
      else if (c == 27) go_idle();
    } else {
      if (c == 'n' || c == 'N') cycle_tier(+1);
      if (c == 'p' || c == 'P') cycle_tier(-1);
      if (c == '1') apply_game(0, true);
      if (c == '2') apply_game(1, true);
      if (c == 'a' || c == 'A') open_admin_gate();
      if (c == 'c' || c == 'C') open_touch_cal();
      if (c == 'r' || c == 'R') {
        reset_progress();
        ble_publish();
        Serial.println("progress reset");
      }
    }
  }

  static uint32_t s_next_frame = 0;
  if ((int32_t)(now - s_next_frame) >= 0) {
    s_next_frame = now + FRAME_MS;
    render_frame(now);
  }
}
