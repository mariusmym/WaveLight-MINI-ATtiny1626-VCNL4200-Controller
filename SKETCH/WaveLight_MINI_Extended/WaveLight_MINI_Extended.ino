/*
  WaveLight MINI - ATtiny1626 + VCNL4200 - Fancy features

  AUTOMATIC MODE (dark + someone near):
    - Lights fade in, stay on while someone is still in range (re-trigger),
      then dim for a few seconds as a "still there?" warning, then fade out.

  TOUCH GESTURES (hand a few mm from the sensor):
    - TAP while ON at full brightness -> DIM
    - TAP while DIM                   -> OFF (manual mode: sensor ignored for 1 min)
    - TAP while OFF                   -> ON (works even in daylight)
    - HOLD while ON                   -> cycle colours / effects (one step per second),
                                         release on the one you like. It is saved in EEPROM.

  COLOURS: cool white, warm white, amber, red, ice blue, purple, candle (effect), rainbow (effect)
*/

#include <tinyNeoPixel_Static.h>
#include <Vishay_VCNL4200.h>
#include <EEPROM.h>

// ---------------- Strip ----------------
#define NUMLEDS 44                  // Number of LEDs in the strip
#define LED_PIN PIN_PA7

// ---------------- Sensor thresholds ----------------
#define PROXIMITY_THRESHOLD     30   // Presence. 30cm ~ 180 ; 40cm ~ 120 ; 50cm ~ 70; 60cm ~ 40 ; 70cm ~ 26; 80cm ~ 20; 100cm ~ 12
#define TOUCH_THRESHOLD         1000 // "Touch" (a few mm)
#define TOUCH_RELEASE           750  // Hand must drop below this to count as released (hysteresis)
#define AMBIENT_LIGHT_THRESHOLD 2    // Below this = dark. Ceiling lights on ~ 11000+ ; TV only ~ 13 ; minimum = 2

// ---------------- Brightness ----------------
#define FULL_BRIGHTNESS 100          // 0-255
#define DIM_BRIGHTNESS  15           // 0-255, used by the first tap and by the "about to turn off" warning
#define FADE_STEP       4            // Brightness units per loop tick. Higher = faster fade (4 -> ~0.5 s)

// ---------------- Timing (ms) ----------------
#define LOOP_MS            20
#define AUTO_ON_TIME       60000UL   // Auto mode: stay on this long after the last presence
#define WARN_TIME          10000UL   // ...then stay dimmed this long before turning off
#define MANUAL_ON_TIME     900000UL  // Lights turned on by a tap switch off after 15 min (0 = never)
#define OVERRIDE_DURATION  60000UL   // After a tap-OFF, the sensor is ignored this long
#define HOLD_TIME          700UL     // Touch longer than this = hold (colour change)
#define COLOR_STEP_MS      1000UL    // While holding, next colour every ...
#define TAP_GUARD_MS       1500UL    // Ignore a tap right after auto-on (the approaching hand triggered it)
#define RETRIGGER          true      // true = presence keeps the lights on in auto mode

// ---------------- Colours ----------------
struct Color { uint8_t r, g, b; };
const Color PRESETS[] = {
  {255, 255, 255},   // 0 cool white (same as the original sketch)
  {255, 170,  80},   // 1 warm white
  {255,  90,   0},   // 2 amber
  {255,   0,   0},   // 3 red (keeps your night vision)
  { 60, 140, 255},   // 4 ice blue
  {180,   0, 255},   // 5 purple
};
#define NUM_PRESETS (sizeof(PRESETS) / sizeof(PRESETS[0]))
#define FX_CANDLE   (NUM_PRESETS)        // flickering warm light
#define FX_RAINBOW  (NUM_PRESETS + 1)    // slowly moving rainbow
#define NUM_COLORS  (NUM_PRESETS + 2)

// ---------------- EEPROM ----------------
#define EE_MAGIC_ADDR 0
#define EE_COLOR_ADDR 1
#define EE_MAGIC      0x57

// ---------------- Strip buffer ----------------
byte pixels[NUMLEDS * 3];            // 3 bytes per pixel for WS2812B (RGB)
tinyNeoPixel leds = tinyNeoPixel(NUMLEDS, LED_PIN, NEO_GRB + NEO_KHZ800, pixels);

// ---------------- State ----------------
enum State : uint8_t { S_OFF, S_AUTO_ON, S_AUTO_WARN, S_MANUAL_ON, S_MANUAL_OFF };
State state = S_OFF;
bool dimmed = false;                 // first tap dims
uint8_t colorIdx = 0;
uint8_t curBright = 0;               // brightness currently shown (fades toward the target)
bool dirty = true;                   // strip needs a redraw
uint8_t fxPhase = 0;                 // animation counter

unsigned long stateTime = 0;         // when the current state started (or was last refreshed)
unsigned long autoOnTime = 0;

uint16_t prx;
uint16_t lux;
bool dark = false;
bool ledsWereOff = false;
unsigned long offSince = 0;
unsigned long lastLuxRead = 0;

bool touching = false;
bool holdUsed = false;
bool tapIgnored = false;
unsigned long touchStart = 0;
unsigned long nextColorStep = 0;

// ================= Sensor (only place that talks to the VCNL) =================
uint16_t readProximity() {
  uint16_t v;
  vcnl4200.read_PRX(&v);
  return v;
}

uint16_t readLux() {
  return (uint16_t)vcnl4200.get_lux();
}

// ================= Setup / loop =================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  vcnl4200.begin();
  leds.begin();
  loadSettings();
  render();                          // make sure the strip starts dark
  delay(1000);
}

void loop() {
  prx = readProximity();
  updateDarkness();
  handleAuto();                      // before touch, so an auto-on caused by the approaching hand is seen first
  handleTouch();
  updateLeds();
  delay(LOOP_MS);
}

// ================= Logic =================
void setState(State s) {
  state = s;
  stateTime = millis();
}

bool lightsOn() {
  return state == S_AUTO_ON || state == S_MANUAL_ON;
}

// Ambient light is only measured while the strip is fully off,
// otherwise the sensor would see our own LEDs.
void updateDarkness() {
  unsigned long now = millis();
  if (curBright > 0) {
    ledsWereOff = false;
    return;
  }
  if (!ledsWereOff) {
    ledsWereOff = true;
    offSince = now;
  }
  if (now - offSince > 300 && now - lastLuxRead > 250) {
    lastLuxRead = now;
    lux = readLux();
    dark = lux < AMBIENT_LIGHT_THRESHOLD;
  }
}

void handleAuto() {
  unsigned long now = millis();
  bool presence = prx > PROXIMITY_THRESHOLD;

  switch (state) {
    case S_OFF:
      if (presence && dark) {
        dimmed = false;
        setState(S_AUTO_ON);
        autoOnTime = now;
      }
      break;

    case S_AUTO_ON:
      if (RETRIGGER && presence) stateTime = now;
      if (now - stateTime > AUTO_ON_TIME) setState(S_AUTO_WARN);
      break;

    case S_AUTO_WARN:                // dimmed "still there?" phase
      if (presence) setState(S_AUTO_ON);
      else if (now - stateTime > WARN_TIME) setState(S_OFF);
      break;

    case S_MANUAL_ON:
      if (MANUAL_ON_TIME > 0 && now - stateTime > MANUAL_ON_TIME) setState(S_OFF);
      break;

    case S_MANUAL_OFF:
      if (now - stateTime > OVERRIDE_DURATION) setState(S_OFF);
      break;
  }
}

void handleTouch() {
  unsigned long now = millis();

  if (!touching) {
    if (prx > TOUCH_THRESHOLD) {     // touch starts
      touching = true;
      holdUsed = false;
      touchStart = now;
      nextColorStep = now + HOLD_TIME;
      tapIgnored = (state == S_AUTO_ON && now - autoOnTime < TAP_GUARD_MS);
    }
    return;
  }

  if (prx < TOUCH_RELEASE) {         // touch ends
    touching = false;
    if (holdUsed) saveSettings();
    else if (!tapIgnored && now - touchStart < HOLD_TIME) onTap();
    return;
  }

  // Still holding: cycle colours while the lights are on
  if (lightsOn() && (long)(now - nextColorStep) >= 0) {
    colorIdx = (colorIdx + 1) % NUM_COLORS;
    nextColorStep = now + COLOR_STEP_MS;
    holdUsed = true;
    dirty = true;
    stateTime = now;                 // don't time out while choosing a colour
  }
}

void onTap() {
  switch (state) {
    case S_AUTO_ON:
    case S_MANUAL_ON:
    case S_AUTO_WARN:
      if (!dimmed && state != S_AUTO_WARN) {
        dimmed = true;               // 1st tap: dim
        stateTime = millis();
      } else {
        setState(S_MANUAL_OFF);      // 2nd tap: off, ignore the sensor for a while
      }
      break;

    case S_OFF:
    case S_MANUAL_OFF:
      dimmed = false;
      setState(S_MANUAL_ON);         // tap turns it on, even in daylight
      break;
  }
}

// ================= LEDs =================
uint8_t targetBrightness() {
  switch (state) {
    case S_AUTO_ON:
    case S_MANUAL_ON:  return dimmed ? DIM_BRIGHTNESS : FULL_BRIGHTNESS;
    case S_AUTO_WARN:  return dimmed ? DIM_BRIGHTNESS / 3 : DIM_BRIGHTNESS;
    default:           return 0;
  }
}

void updateLeds() {
  uint8_t target = targetBrightness();

  if (curBright < target) {
    curBright = (target - curBright > FADE_STEP) ? curBright + FADE_STEP : target;
    dirty = true;
  } else if (curBright > target) {
    curBright = (curBright - target > FADE_STEP) ? curBright - FADE_STEP : target;
    dirty = true;
  }

  if (curBright > 0) {
    fxPhase++;
    if (colorIdx == FX_RAINBOW) dirty = true;                          // every tick
    else if (colorIdx == FX_CANDLE && (fxPhase & 3) == 0) dirty = true; // every 4th tick
  }

  if (dirty) {
    render();
    dirty = false;
  }
}

uint8_t scale8(uint8_t c, uint8_t b) {
  return ((uint16_t)c * b) / 255;
}

void wheel(uint8_t pos, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (pos < 85)       { r = 255 - pos * 3; g = pos * 3;       b = 0; }
  else if (pos < 170) { pos -= 85;  r = 0; g = 255 - pos * 3; b = pos * 3; }
  else                { pos -= 170; r = pos * 3; g = 0;       b = 255 - pos * 3; }
}

void render() {
  for (uint16_t i = 0; i < NUMLEDS; i++) {
    uint8_t r, g, b;
    if (colorIdx == FX_RAINBOW) {
      wheel((uint8_t)(fxPhase + i * (256 / NUMLEDS)), r, g, b);
    } else if (colorIdx == FX_CANDLE) {
      uint8_t f = 170 + random(86);  // 170..255 flicker per LED
      r = f;
      g = scale8(120, f);
      b = scale8(20, f);
    } else {
      r = PRESETS[colorIdx].r;
      g = PRESETS[colorIdx].g;
      b = PRESETS[colorIdx].b;
    }
    leds.setPixelColor(i, scale8(r, curBright), scale8(g, curBright), scale8(b, curBright));
  }
  leds.show();
}

// ================= EEPROM =================
void loadSettings() {
  if (EEPROM.read(EE_MAGIC_ADDR) == EE_MAGIC) {
    uint8_t c = EEPROM.read(EE_COLOR_ADDR);
    if (c < NUM_COLORS) colorIdx = c;
  }
}

void saveSettings() {
  EEPROM.update(EE_MAGIC_ADDR, EE_MAGIC);
  EEPROM.update(EE_COLOR_ADDR, colorIdx);
}
