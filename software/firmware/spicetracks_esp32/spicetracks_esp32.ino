// Spicetracks ESP32 firmware
// Sensors, touch, pump, distributor and the LED strip in the roof.
// Talks to the Raspberry Pi over USB serial: one JSON message per line, 115200 baud.
//
// Board: ESP32-S3-DevKitC-1 (Arduino: "ESP32S3 Dev Module", USB CDC On Boot: Disabled, USB cable in the UART port).
// Libraries: Adafruit NeoPixel (drives the strip), ArduinoJson (v7). No FastLED any more: colour maths in led_colors.h.
// Pins follow the team's Steckplan.

#include <Arduino.h>
#include <esp_arduino_version.h>
#include <esp_idf_version.h>
#include <Adafruit_NeoPixel.h>
#include "led_colors.h"
#include <ArduinoJson.h>
#include "driver/gpio.h"
#undef RGB_BUILTIN                                 // board LED stays off: the RMT unit is only for the strip
#include "touch_lukas.h"                           // Lukas' touch detection, runs on core 0

// ---------------- pins ----------------
const uint8_t PIN_MOIST[4] = {1, 2, 4, 5};        // ADC1, plant 1..4
const uint8_t PIN_SENSOR_PWR = 10;                 // switched 3.3 V for the moisture sensors (top red rail of breadboard 1)
const uint8_t PIN_SENSOR_GND = 9;                  // switched GND for the moisture sensors (top blue rail of breadboard 1, no bridge V4)
// Between measurements both pins are inputs: the sensors float and are no longer a large grounded plate in the soil.
const uint8_t PIN_LDR = 6;                         // later: LDR to 3V3, 10k to GND
const uint8_t PIN_TANK = 7;                        // later: float switch to GND or 5th moisture sensor
// touch pins 11, 12, 13, 14 (each through 1 kOhm to the screw in the soil): see touch_lukas.h
const uint8_t PIN_RELAY = 21;                      // relay module S, active HIGH, 10k pull-down on the breadboard
const uint8_t PIN_STEP[4] = {15, 16, 17, 18};      // ULN2003 IN1..IN4
const uint8_t PIN_ENDSTOP = 39;                    // later: to GND, INPUT_PULLUP
const uint8_t PIN_LED = 40;                        // SK6812 RGBW data, 330 ohm in series

// ---------------- settings ----------------
const bool TANK_SENSOR = false;                    // set true once a tank sensor is fitted
const bool TANK_IS_FLOAT = true;                   // true: float switch (LOW = water), false: analog sensor
const uint16_t TANK_DRY_ADC = 2600;                // analog sensor: above this = empty

const uint32_t PUMP_MAX_MS = 10000;                // hard limit per watering
const uint32_t PI_TIMEOUT_MS = 30000;              // no message from the Pi: back to idle light
const uint32_t STATUS_EVERY_MS = 2000;

// Moisture sensors are only switched on for a short measurement: while on, their signal in the wet soil
// makes the touch measurement of the plant time out. Touch pauses during the measurement.
uint32_t moistEveryMs = 600000;                    // every 10 min, the Pi sends config.yaml sensors.moist_every_min
uint32_t moistAfterWaterMs = 45000;                // and once more after watering, when the water has soaked in
const uint32_t MOIST_SETTLE_MS = 1000;             // sensor needs a moment after switching on
const uint32_t MOIST_SAMPLE_MS = 1000;             // then average for 1 s
const uint32_t MOIST_RELEASE_MS = 200;             // sensors off, short wait before touch measures again

const int STEPS_PER_REV = 2048;                    // 28BYJ-48, full steps
int plantSteps[4] = {495, 330, 165, 0};            // distributor position per plant (165 steps apart, 0 = plant 4); the Pi sends config.yaml distributor.steps
const bool HAS_ENDSTOP = false;                    // no end switch yet: distributor must stand at plant 4 (step 0) when powered on
const int STEP_US = 2200;                          // time per step, slower = more torque

// LEDs, one chain: 0..27 in the roof groove (LED 0 at the right end, where the cable comes in),
// then a piece of 3 LEDs inside the trough at each plant: 28-30 plant 1, 31-33 plant 2, 34-36 plant 3, 37-39 plant 4.
const int ROOF_LEDS = 28, POT_LEDS = 3;
const int NUM_LEDS = ROOF_LEDS + 4 * POT_LEDS;
const float LED_X0 = 519.2f, LED_PITCH = -16.667f; // x of LED 0 (mm from the box's left edge) and step to the next
const float PLANT_X[4] = {74.25f, 220.75f, 367.25f, 513.75f};
uint32_t ledMaxMa = 1500;                          // current limit, the Pi can change it (config.yaml: light.max_ma)
bool ledUseWhite = false;                          // true: the white part of a colour goes to the white chip (config.yaml: light.use_white_chip)

// The strips are RGBW (SK6812): 4 bytes per LED. Sending RGB data to them gives the green-blue-red pattern.
// If red and green are swapped, use NEO_RGBW instead of NEO_GRBW.
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED, NEO_GRBW + NEO_KHZ800);
CRGB leds[NUM_LEDS];                               // the light programme draws in RGB, showLeds() turns it into RGBW
uint32_t ledShowUs = 0, ledShowMaxUs = 0, ledFrames = 0;   // how long sending a frame takes, for the status
uint8_t ledZone[NUM_LEDS];                         // nearest plant for every LED
float ledPos[NUM_LEDS];                            // 0..1 along the strip

// ---------------- state ----------------
enum LightMode { MODE_OFF, MODE_IDLE, MODE_ZONE, MODE_EQ };
LightMode mode = MODE_IDLE;
// Colour theme per plant: dark, main, bright. The equalizer runs through these three by loudness.
// These are only the start values: the Pi sends the themes from config.yaml (plants: led:) when it connects.
CRGB PAL[5][3] = {
  {CRGB(0, 45, 8),   CRGB(0, 200, 35),  CRGB(90, 255, 50)},    // 0 Basilikum: deep green, green, fresh lime
  {CRGB(35, 55, 0),  CRGB(120, 200, 0), CRGB(215, 255, 20)},   // 1 Oregano: olive, yellow-green, light yellow-green
  {CRGB(110, 50, 0), CRGB(255, 150, 0), CRGB(255, 215, 40)},   // 2 Currykraut: amber, gold, sunny yellow
  {CRGB(80, 10, 0),  CRGB(230, 45, 0),  CRGB(255, 95, 25)},    // 3 Zimtblätter: dark red-brown, red-orange, coral
  {CRGB(60, 25, 5),  CRGB(255, 140, 50), CRGB(255, 200, 120)}, // 4 music without a plant (started from the phone)
};
inline int palOf(int plant) { return (plant >= 0 && plant < 4) ? plant : 4; }

// 0..1 through dark -> main -> bright
CRGB palAt(int p, float x) {
  x = constrain(x, 0.0f, 1.0f);
  if (x < 0.5f) return blend(PAL[p][0], PAL[p][1], (uint8_t)(x * 2 * 255));
  return blend(PAL[p][1], PAL[p][2], (uint8_t)((x - 0.5f) * 2 * 255));
}
int activePlant = -1;
uint8_t brightness = 150;                          // 0..255, set by the Pi
uint8_t eqBands[8] = {0};
uint32_t beatAt = 0;                               // last beat from the Pi, the roof flares up briefly
uint32_t eqLastMs = 0;

bool pumpOn = false;
uint32_t pumpUntil = 0, pumpStart = 0;
int pumpPlant = -1;

int stepPos = 0;                                   // current distributor position in steps
bool homed = false;
uint8_t stepPhase = 0;


enum MoistPhase { MP_IDLE, MP_PAUSE, MP_ON, MP_SAMPLE, MP_OFF };
MoistPhase moistPhase = MP_IDLE;
uint32_t moistPhaseAt = 0, moistNextAt = 0, moistLastAt = 0, moistN = 0;
uint32_t moistSum[4] = {0};
uint16_t moistVal[4] = {0};
bool moistValid = false;                           // false until the first measurement after the start

uint32_t lastPiMs = 0, lastStatusMs = 0;
uint32_t touchMonUntil = 0;
// Arduino-ESP32 3.3 and newer (ESP-IDF 5.5) use a new touch driver that counts about 100 times higher:
// with its default of 500 charge cycles the plants run into the maximum (4194303). Fewer cycles keep it in range.
const uint32_t TOUCH_CHG_TIMES = 40;                       // live touch values for the hardware test, see sendTouchMon()
const uint16_t TOUCH_MEAS_CYCLES = 100;                    // core 3.2.x: fewer charge cycles, wet soil stays below the timeout (tested 30.10.)
String rx;

// ---------------- helpers ----------------
void send(JsonDocument &doc) {
  serializeJson(doc, Serial);
  Serial.print('\n');
}

void sendError(const char *msg) {
  JsonDocument d;
  d["t"] = "error";
  d["msg"] = msg;
  send(d);
}

uint16_t readAvg(uint8_t pin, int n = 8) {
  uint32_t s = 0;
  for (int i = 0; i < n; i++) s += analogRead(pin);
  return s / n;
}

bool tankEmpty() {
  if (!TANK_SENSOR) return false;
  if (TANK_IS_FLOAT) return digitalRead(PIN_TANK) == HIGH;   // float open = no water
  return readAvg(PIN_TANK) > TANK_DRY_ADC;
}

// relay: active HIGH (signal S, 10k pull-down keeps it off while the ESP32 boots)
void relayOn() { pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, HIGH); }
void relayOff() { pinMode(PIN_RELAY, OUTPUT); digitalWrite(PIN_RELAY, LOW); }

// ---------------- stepper ----------------
const uint8_t SEQ[4][4] = {{1, 0, 0, 1}, {1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 1}};

void stepCoils(uint8_t phase) {
  for (int i = 0; i < 4; i++) digitalWrite(PIN_STEP[i], SEQ[phase][i]);
}

void coilsOff() {
  for (int i = 0; i < 4; i++) digitalWrite(PIN_STEP[i], LOW);   // otherwise the motor gets hot
}

void stepOnce(int dir) {
  stepPhase = (stepPhase + (dir > 0 ? 1 : 3)) & 3;
  stepCoils(stepPhase);
  stepPos += dir;
  delayMicroseconds(STEP_US);
}

void moveTo(int target);
void showLeds();
void ledSelfTest();

bool home() {
  if (!HAS_ENDSTOP) {                                   // no switch: drive back to the start position we counted from
    moveTo(0);
    homed = true;
    return true;
  }
  // turn backwards until the end switch closes, at most 1.2 turns
  for (int i = 0; i < STEPS_PER_REV * 6 / 5; i++) {
    if (digitalRead(PIN_ENDSTOP) == LOW) {
      stepPos = 0;
      homed = true;
      coilsOff();
      return true;
    }
    stepOnce(-1);
    if (i % 64 == 0) showLeds();       // keep animations alive a little
  }
  coilsOff();
  homed = false;
  return false;
}

void moveTo(int target) {
  int dir = target > stepPos ? 1 : -1;
  while (stepPos != target) stepOnce(dir);
  coilsOff();
}

// ---------------- watering ----------------
void startWater(int plant, uint32_t ms) {
  if (plant < 0 || plant > 3) { sendError("bad_plant"); return; }
  if (pumpOn) { sendError("busy"); return; }
  if (tankEmpty()) { sendError("tank_empty"); return; }
  if (!homed && !home()) { sendError("home_failed"); return; }
  moveTo(plantSteps[plant]);
  ms = min(ms, PUMP_MAX_MS);
  afterWatering();                                      // no touch events while the water runs
  pumpPlant = plant;
  pumpStart = millis();
  pumpUntil = pumpStart + ms;
  pumpOn = true;
  relayOn();
}

void stopWater(bool report) {
  if (!pumpOn) return;
  relayOff();
  pumpOn = false;
  if (report) {
    JsonDocument d;
    d["t"] = "watered";
    d["plant"] = pumpPlant;
    d["ms"] = millis() - pumpStart;
    send(d);
  }
  afterWatering();                                      // touch: 60 s pause, then 30 s new calibration (Lukas)
  moistNextAt = millis() + moistAfterWaterMs;           // measure the moisture again once the water has soaked in
}

// ---------------- moisture measurement ----------------
void sendStatus();

void sensorsOn() {
  pinMode(PIN_SENSOR_GND, OUTPUT);
  digitalWrite(PIN_SENSOR_GND, LOW);
  gpio_set_drive_capability((gpio_num_t)PIN_SENSOR_GND, GPIO_DRIVE_CAP_3);   // takes the current of four sensors
  pinMode(PIN_SENSOR_PWR, OUTPUT);
  digitalWrite(PIN_SENSOR_PWR, HIGH);
  gpio_set_drive_capability((gpio_num_t)PIN_SENSOR_PWR, GPIO_DRIVE_CAP_3);
}

void sensorsOff() {
  digitalWrite(PIN_SENSOR_PWR, LOW);
  pinMode(PIN_SENSOR_PWR, INPUT);                       // no pull-up, no pull-down: floating
  pinMode(PIN_SENSOR_GND, INPUT);
}

// runs in loop(), never blocks: pause touch, sensors on, wait, average, sensors off, touch on again
void moistTick(uint32_t now) {
  switch (moistPhase) {
    case MP_IDLE:
      if ((int32_t)(now - moistNextAt) >= 0 && !touchCalibrating && !pumpOn) {
        touchPauseReq = true;
        moistPhase = MP_PAUSE; moistPhaseAt = now;
      }
      break;
    case MP_PAUSE:                                      // wait until the touch task has stopped measuring
      if (touchPaused) {
        sensorsOn();
        moistPhase = MP_ON; moistPhaseAt = now;
        sendStatus();                                   // screen: moisture sensors on
      } else if (now - moistPhaseAt > 500) {            // touch is calibrating right now: try again in 5 s
        touchPauseReq = false;
        moistNextAt = now + 5000;
        moistPhase = MP_IDLE;
      }
      break;
    case MP_ON:
      if (now - moistPhaseAt >= MOIST_SETTLE_MS) {
        for (int i = 0; i < 4; i++) moistSum[i] = 0;
        moistN = 0;
        moistPhase = MP_SAMPLE; moistPhaseAt = now;
      }
      break;
    case MP_SAMPLE:
      for (int i = 0; i < 4; i++) moistSum[i] += analogRead(PIN_MOIST[i]);
      moistN++;
      if (now - moistPhaseAt >= MOIST_SAMPLE_MS) {
        for (int i = 0; i < 4; i++) moistVal[i] = moistSum[i] / max(moistN, (uint32_t)1);
        moistValid = true;
        moistLastAt = now;
        sensorsOff();                                   // sensors off and floating again
        moistPhase = MP_OFF; moistPhaseAt = now;
      }
      break;
    case MP_OFF:
      if (now - moistPhaseAt >= MOIST_RELEASE_MS) {
        touchPauseReq = false;                          // touch measures again
        moistNextAt = now + moistEveryMs;
        moistPhase = MP_IDLE;
        sendStatus();                                   // new values right away
      }
      break;
  }
}

// ---------------- LEDs ----------------
// RGB -> RGBW, brightness, current limit, then out to the strip
void showLeds() {
  static uint8_t px[NUM_LEDS][4];
  uint32_t load = 0;                                    // mA * 255 at full brightness
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t r = leds[i].r, g = leds[i].g, b = leds[i].b, w = 0;
    if (ledUseWhite) {
      w = min(r, min(g, b));
      r -= w; g -= w; b -= w;
    }
    px[i][0] = r; px[i][1] = g; px[i][2] = b; px[i][3] = w;
    load += r * 16u + g * 12u + b * 15u + w * 20u;     // rough mA per channel at full
  }
  uint32_t ma = load * brightness / (255u * 255u);
  uint8_t bri = brightness;
  if (ma > ledMaxMa) bri = (uint8_t)((uint32_t)brightness * ledMaxMa / ma);
  for (int i = 0; i < NUM_LEDS; i++)
    strip.setPixelColor(i, scale8_video(px[i][0], bri), scale8_video(px[i][1], bri),
                        scale8_video(px[i][2], bri), scale8_video(px[i][3], bri));
  uint32_t t0 = micros();
  strip.show();
  ledShowUs = micros() - t0;                            // normal: about 2000 us. About 50000: the library skips frames
  if (ledShowUs > ledShowMaxUs) ledShowMaxUs = ledShowUs;
  ledFrames++;
}

// red, green, blue, white for 0.4 s each, then dark. Runs at every start and with {"cmd":"ledtest"}.
void ledSelfTest() {
  const CRGB seq[4] = {CRGB(255, 0, 0), CRGB(0, 255, 0), CRGB(0, 0, 255), CRGB(255, 255, 255)};
  uint8_t keep = brightness;
  brightness = 60;
  for (int k = 0; k < 4; k++) {
    fill_solid(leds, NUM_LEDS, seq[k]);
    showLeds();
    delay(400);
  }
  fill_solid(leds, NUM_LEDS, CRGB());
  showLeds();
  brightness = keep;
}

void setupLeds() {
  strip.begin();
  strip.clear();
  strip.show();
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i >= ROOF_LEDS) {                               // the 3-LED pieces at the plants
      ledZone[i] = (i - ROOF_LEDS) / POT_LEDS;
      ledPos[i] = -1;
      continue;
    }
    float x = LED_X0 + i * LED_PITCH;
    ledPos[i] = (x - PLANT_X[0]) / (PLANT_X[3] - PLANT_X[0]);   // 0 left .. 1 right, independent of wiring direction
    int best = 0;
    for (int p = 1; p < 4; p++) if (fabsf(x - PLANT_X[p]) < fabsf(x - PLANT_X[best])) best = p;
    ledZone[i] = best;
  }
}

// ---------------- light concept ----------------
// Roof, no music: the sun moves across the day (time comes from the Pi), at night a faint moon.
// Roof, music: light spreads from the plant above which it was started, then the equalizer; on pause it shrinks back.
// Plant pieces, no music: the state of the plant (ok green, thirsty orange pulse, very wet blue, no value dim white).
// Plant pieces, music: the playing plant pulses with the bass, the others glow softly in their state colour.
// Touch: a short greeting flash in the plant colour. Watering: blue drops run down the plant piece.

enum PlantState { PS_NONE, PS_OK, PS_DRY, PS_WET };
PlantState plantState[4] = {PS_NONE, PS_NONE, PS_NONE, PS_NONE};
float sunHour = 12.0f;                               // local time, set by the Pi every minute
uint32_t sunHourAt = 0;
uint32_t flashUntil[4] = {0};
uint32_t animStart = 0;                              // start (spread) or stop (shrink) animation
bool animShrink = false;
int animPlant = 0;
const uint32_t SPREAD_MS = 900, SHRINK_MS = 700, FLASH_MS = 450;

float plantPos(int p) { return (PLANT_X[p] - PLANT_X[0]) / (PLANT_X[3] - PLANT_X[0]); }

CRGB stateColor(int p, uint32_t now) {
  switch (plantState[p]) {
    case PS_OK:  return CRGB(30, 190, 50).nscale8_video(150);
    case PS_DRY: return CRGB(255, 110, 0).nscale8_video(beatsin8(14, 40, 220, 0, p * 40));
    case PS_WET: return CRGB(60, 130, 255).nscale8_video(140);
    default:     return CRGB(255, 240, 220).nscale8_video(25);
  }
}

// colour and position of the sun (or moon) for an hour of the day
void skyAt(float h, float &pos, CRGB &col, float &width, uint8_t &ambient, CRGB &ambCol) {
  if (h >= 6.0f && h < 20.5f) {                      // day: from left (east) to right (west)
    pos = (h - 6.0f) / 14.5f;
    width = 0.16f;
    if (h < 8.5f)       col = blend(CRGB(255, 120, 60), CRGB(255, 200, 140), (uint8_t)((h - 6.0f) / 2.5f * 255));
    else if (h < 17.0f) col = CRGB(255, 214, 160);
    else                col = blend(CRGB(255, 200, 140), CRGB(255, 70, 20), (uint8_t)((h - 17.0f) / 3.5f * 255));
    ambient = 28; ambCol = CRGB(255, 170, 90);
  } else {                                           // night: a small, cool moon
    float n = (h >= 20.5f ? h - 20.5f : h + 3.5f) / 9.5f;
    pos = n;
    width = 0.09f;
    col = CRGB(90, 120, 200);
    ambient = 8; ambCol = CRGB(40, 60, 140);
  }
}

void renderRoofIdle(uint32_t now) {
  float h = sunHour + (now - sunHourAt) / 3600000.0f;
  if (h >= 24) h -= 24;
  float pos, width; CRGB col, amb; uint8_t ambient;
  skyAt(h, pos, col, width, ambient, amb);
  uint8_t breathe = beatsin8(5, 200, 255);           // the sun breathes very slightly
  for (int i = 0; i < ROOF_LEDS; i++) {
    float d = (ledPos[i] - pos) / width;
    float g = expf(-d * d);
    CRGB c = amb; c.nscale8_video(ambient);
    CRGB s = col; s.nscale8_video((uint8_t)(g * breathe));
    leds[i] = c + s;
  }
}

void renderEq(int p, uint32_t now) {
  // spectrum mirrored from the centre: bass in the middle, treble at both ends.
  // Quiet = dark theme colour, loud = bright theme colour, on a beat the roof flares towards the bright colour.
  float beat = (now - beatAt) < 160 ? 1.0f - (now - beatAt) / 160.0f : 0.0f;
  for (int i = 0; i < ROOF_LEDS; i++) {
    float d = constrain(fabsf(ledPos[i] - 0.5f) * 2.0f, 0.0f, 1.0f) * 7.0f;   // 0..7 band index
    int b0 = (int)d, b1 = min(b0 + 1, 7);
    float f = d - b0;
    float lvl = (eqBands[b0] * (1 - f) + eqBands[b1] * f) / 255.0f;
    CRGB c = palAt(p, lvl + 0.35f * beat);
    c.nscale8_video((uint8_t)(35 + 220 * max(lvl, 0.6f * beat)));
    leds[i] = c;
  }
}

void renderRoofMusic(uint32_t now) {
  int p = palOf(activePlant);
  CRGB base = PAL[p][1];
  uint32_t t = now - animStart;
  if (t < SPREAD_MS) {                                // light grows from the plant to both ends
    float r = (float)t / SPREAD_MS * 1.1f;
    float c0 = plantPos(activePlant >= 0 ? activePlant : 0);
    for (int i = 0; i < ROOF_LEDS; i++) {
      float d = fabsf(ledPos[i] - c0);
      leds[i] = base;
      leds[i].nscale8_video(d < r ? (uint8_t)(255 - 180 * d / max(r, 0.01f)) : 0);
    }
  } else if (mode == MODE_EQ) {
    renderEq(p, now);
  } else {                                            // waiting for sound: the theme colours drift slowly over the roof
    for (int i = 0; i < ROOF_LEDS; i++) {
      leds[i] = palAt(p, 0.35f + 0.35f * sinf(now / 900.0f + ledPos[i] * 6.0f));
      leds[i].nscale8_video(beatsin8(24, 90, 160));
    }
  }
}

void renderRoofShrink(uint32_t now) {                 // music stopped: light pulls back into the plant
  float k = 1.0f - (float)(now - animStart) / SHRINK_MS;
  float c0 = plantPos(animPlant);
  CRGB base = PAL[palOf(animPlant)][1];
  for (int i = 0; i < ROOF_LEDS; i++) {
    float d = fabsf(ledPos[i] - c0);
    float r = k * 1.1f;
    leds[i] = base;
    leds[i].nscale8_video(d < r ? (uint8_t)(200 * k) : 0);
  }
}

void renderPots(uint32_t now, bool music) {
  for (int p = 0; p < 4; p++) {
    int first = ROOF_LEDS + p * POT_LEDS;
    for (int j = 0; j < POT_LEDS; j++) {
      CRGB c;
      if (pumpOn && pumpPlant == p) {                 // watering: drops running down
        int k = (now / 160 + POT_LEDS - j) % (POT_LEDS + 1);
        c = (k == 0) ? CRGB(80, 170, 255) : CRGB(0, 25, 70);
      } else if (music && p == activePlant) {         // playing plant: bass pulse
        uint8_t lvl = max(eqBands[0], eqBands[1]);
        c = mode == MODE_EQ ? palAt(p, lvl / 255.0f) : PAL[p][1];
        c.nscale8_video(mode == MODE_EQ ? 50 + lvl * 205 / 255 : beatsin8(24, 90, 200));
      } else {
        c = stateColor(p, now);
        if (music) c.nscale8_video(110);              // others step back while music plays
      }
      if (now < flashUntil[p]) {                      // greeting when the plant is touched
        uint8_t f = 255 * (flashUntil[p] - now) / FLASH_MS;
        c = blend(c, PAL[p][2], f);                   // bright theme colour, not white
      }
      leds[first + j] = c;
    }
  }
}

void renderLeds() {
  uint32_t now = millis();
  // EQ frames stop coming (music paused, Pi busy): back to the soft glow of the playing plant
  if (mode == MODE_EQ && (int32_t)(now - eqLastMs) > 600) mode = MODE_ZONE;

  bool music = (mode == MODE_ZONE || mode == MODE_EQ);
  if (mode == MODE_OFF) {
    fill_solid(leds, NUM_LEDS, CRGB());
  } else {
    if (music) renderRoofMusic(now);
    else if (animShrink && now - animStart < SHRINK_MS) renderRoofShrink(now);
    else renderRoofIdle(now);
    renderPots(now, music);
    for (int p = 0; p < 4; p++)                       // touch greeting also lights the roof above the plant
      if (now < flashUntil[p] && !music)
        for (int i = 0; i < ROOF_LEDS; i++)
          if (ledZone[i] == p) leds[i] += CRGB(PAL[p][1]).nscale8_video(200 * (flashUntil[p] - now) / FLASH_MS);
  }
  showLeds();
}

void greet(int plant) {
  if (plant >= 0 && plant < 4) flashUntil[plant] = millis() + FLASH_MS;
}

// ---------------- commands from the Pi ----------------
void handleLine(const String &line) {
  JsonDocument d;
  if (deserializeJson(d, line)) {
    JsonDocument e;
    e["t"] = "error";
    e["msg"] = "bad_json";
    e["line"] = line.substring(0, 60);                  // what arrived, for finding the cause
    send(e);
    return;
  }
  lastPiMs = millis();
  const char *cmd = d["cmd"] | "";

  if (!strcmp(cmd, "eq")) {                             // {"cmd":"eq","b":[8 values 0..255],"beat":1}
    JsonArray b = d["b"];
    for (int i = 0; i < 8 && i < (int)b.size(); i++) eqBands[i] = b[i];
    if (d["beat"] | 0) beatAt = millis();
    eqLastMs = millis();
    if (mode == MODE_IDLE && activePlant < 0) { animStart = millis(); animShrink = false; }
    if (mode != MODE_OFF) mode = MODE_EQ;
  } else if (!strcmp(cmd, "mode")) {                    // {"cmd":"mode","mode":"idle|zone|eq|off","plant":2}
    const char *m = d["mode"] | "idle";
    bool wasMusic = (mode == MODE_ZONE || mode == MODE_EQ);
    int newPlant = d["plant"] | activePlant;
    if (!strcmp(m, "off")) mode = MODE_OFF;
    else if (!strcmp(m, "zone") || !strcmp(m, "eq")) {
      if (!wasMusic || newPlant != activePlant) { animStart = millis(); animShrink = false; }   // spread from this plant
      activePlant = newPlant;
      mode = !strcmp(m, "eq") ? MODE_EQ : MODE_ZONE;
      eqLastMs = millis();
    } else {
      if (wasMusic && activePlant >= 0) { animStart = millis(); animShrink = true; animPlant = activePlant; }
      mode = MODE_IDLE; activePlant = -1;
    }
  } else if (!strcmp(cmd, "color")) {                   // old: screen colour, the LEDs use "theme" instead
  } else if (!strcmp(cmd, "theme")) {                   // {"cmd":"theme","plant":0,"dark":[0,45,8],"main":[0,200,35],"bright":[90,255,50]}
    int p = d["plant"] | -1;
    if (p >= 0 && p < 5) {
      const char *keys[3] = {"dark", "main", "bright"};
      for (int k = 0; k < 3; k++) {
        JsonArray c = d[keys[k]];
        if (c.size() == 3) PAL[p][k] = CRGB(c[0] | 0, c[1] | 0, c[2] | 0);
      }
    }
  } else if (!strcmp(cmd, "steps")) {                   // {"cmd":"steps","pos":[0,165,330,495]}
    JsonArray a = d["pos"];
    if (a.size() == 4)
      for (int i = 0; i < 4; i++) plantSteps[i] = constrain((int)(a[i] | 0), -4096, 4096);
  } else if (!strcmp(cmd, "ledcfg")) {                  // {"cmd":"ledcfg","white":false,"max_ma":1500}
    if (d["white"].is<bool>()) ledUseWhite = d["white"];
    if (d["max_ma"].is<int>()) ledMaxMa = constrain((int)d["max_ma"], 100, 4000);
  } else if (!strcmp(cmd, "time")) {                    // {"cmd":"time","h":13.5} local time for the sun
    sunHour = d["h"] | 12.0f;
    sunHourAt = millis();
  } else if (!strcmp(cmd, "plant")) {                   // {"cmd":"plant","plant":1,"state":"dry"}  ok, dry, wet, none
    int p = d["plant"] | -1;
    const char *st = d["state"] | "none";
    if (p >= 0 && p < 4)
      plantState[p] = !strcmp(st, "ok") ? PS_OK : !strcmp(st, "dry") ? PS_DRY : !strcmp(st, "wet") ? PS_WET : PS_NONE;
  } else if (!strcmp(cmd, "flash")) {
    greet(d["plant"] | -1);
  } else if (!strcmp(cmd, "bright")) {                  // {"cmd":"bright","level":0.6}
    brightness = (uint8_t)constrain((float)(d["level"] | 0.6) * 255.0f, 0.0f, 255.0f);
  } else if (!strcmp(cmd, "light")) {                   // old form from the doc: plant + colour + level
    int p = d["plant"] | -1;
    if (p >= 0 && p < 4) {
      activePlant = p;
      mode = MODE_ZONE;
    }
    if (d["level"].is<float>()) brightness = (uint8_t)(255 * constrain((float)d["level"], 0.0f, 1.0f));
  } else if (!strcmp(cmd, "water")) {                   // {"cmd":"water","plant":3,"ms":4000}
    startWater(d["plant"] | -1, d["ms"] | 3000);
  } else if (!strcmp(cmd, "pump")) {                    // test: pump only, distributor stays where it is
    if (pumpOn) { sendError("busy"); return; }
    if (tankEmpty()) { sendError("tank_empty"); return; }
    uint32_t ms = min((uint32_t)(d["ms"] | 1000), PUMP_MAX_MS);
    afterWatering();
    pumpPlant = -1; pumpStart = millis(); pumpUntil = pumpStart + ms; pumpOn = true;
    relayOn();
  } else if (!strcmp(cmd, "goto")) {                    // test: distributor to a plant, no water
    int p = d["plant"] | 0;
    if (p < 0 || p > 3) { sendError("bad_plant"); return; }
    if (!homed && !home()) { sendError("home_failed"); return; }
    moveTo(plantSteps[p]);
    JsonDocument r; r["t"] = "at"; r["plant"] = p; r["steps"] = stepPos; send(r);
  } else if (!strcmp(cmd, "stop")) {
    stopWater(true);
  } else if (!strcmp(cmd, "home")) {
    JsonDocument r;
    r["t"] = "homed";
    r["ok"] = home();
    send(r);
  } else if (!strcmp(cmd, "touchmon")) {                // {"cmd":"touchmon","s":60} live touch values for 60 s
    touchMonUntil = millis() + 1000UL * constrain((int)(d["s"] | 60), 0, 600);
  } else if (!strcmp(cmd, "touchmap")) {                // {"cmd":"touchmap","pins":[11,13,14,12]} touch pin per plant
    JsonArray a = d["pins"];
    if (a.size() == NCH) {
      bool ok = true, same = true;
      int p[NCH];
      for (int i = 0; i < NCH; i++) {
        p[i] = a[i] | -1;
        if (p[i] < 1 || p[i] > 14) ok = false;
        if (p[i] != ch[i].pin) same = false;
      }
      if (!ok) sendError("bad_touchmap");
      else if (!same) {                                 // only when it changes: costs a 30 s calibration
        for (int i = 0; i < NCH; i++) pendingPins[i] = p[i];
        pinsPending = true;
      }
    }
  } else if (!strcmp(cmd, "moist")) {                   // measure the moisture now
    moistNextAt = millis();
  } else if (!strcmp(cmd, "sensecfg")) {                // {"cmd":"sensecfg","every_s":600,"after_water_s":45}
    if (d["every_s"].is<int>()) {
      uint32_t newEvery = 1000UL * constrain((int)d["every_s"], 60, 86400);
      if (moistPhase == MP_IDLE && moistValid) moistNextAt = moistLastAt + newEvery;
      moistEveryMs = newEvery;
    }
    if (d["after_water_s"].is<int>()) moistAfterWaterMs = 1000UL * constrain((int)d["after_water_s"], 5, 600);
  } else if (!strcmp(cmd, "touchcal")) {
    needRecal = true;                                   // 30 s, nobody may touch the plants
  } else if (!strcmp(cmd, "ledtest")) {
    ledSelfTest();
  } else if (!strcmp(cmd, "ping")) {
    JsonDocument r;
    r["t"] = "pong";
    send(r);
  }
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (rx.length()) handleLine(rx);
      rx = "";
    } else if (c != '\r' && rx.length() < 400) {
      rx += c;
    }
  }
}

void sendStatus() {
  JsonDocument d;
  d["t"] = "status";
  JsonArray m = d["moist"].to<JsonArray>();               // last measurement, null until the first one
  for (int i = 0; i < 4; i++) {
    if (moistValid) m.add(moistVal[i]);
    else m.add<JsonVariant>();             // null
  }
  d["moistAge"] = moistValid ? (int32_t)((millis() - moistLastAt) / 1000) : -1;   // seconds since then
  d["sense"] = moistPhase == MP_ON || moistPhase == MP_SAMPLE;                    // sensors on right now
  d["light"] = readAvg(PIN_LDR);
  d["tank"] = tankEmpty() ? 0 : 1;
  JsonArray t = d["touch"].to<JsonArray>();
  for (int i = 0; i < NCH; i++) t.add(ch[i].touched ? 1 : 0);
  JsonArray ts = d["tst"].to<JsonArray>();              // touch per plant: ok, cal, pause, lock, stuck
  for (int i = 0; i < NCH; i++) ts.add(touchStateOf(i));
  d["touchcal"] = (bool)touchCalibrating;
  d["pump"] = pumpOn;
  d["homed"] = homed;
  d["ledUs"] = ledShowUs;                               // last frame
  d["ledMaxUs"] = ledShowMaxUs;
  d["ledFrames"] = ledFrames;
  ledShowMaxUs = 0;
  send(d);
}

// live view of Lukas' touch values: how far each plant is above its resting value, and the threshold
void sendTouchMon() {
  JsonDocument d;
  d["t"] = "tm";
  JsonArray dl = d["d"].to<JsonArray>(), th = d["thr"].to<JsonArray>(), on = d["on"].to<JsonArray>(), raw = d["raw"].to<JsonArray>();
  JsonArray stk = d["stuck"].to<JsonArray>(), pin = d["pin"].to<JsonArray>();
  for (int i = 0; i < NCH; i++) {
    stk.add(ch[i].stuck ? 1 : 0);
    dl.add(roundf(ch[i].delta * 10) / 10);
    th.add(roundf(ch[i].thrOn * 10) / 10);
    on.add(ch[i].touched ? 1 : 0);
    raw.add(roundf(ch[i].mean));
    pin.add(ch[i].pin);
  }
  d["cal"] = (bool)touchCalibrating;
  d["lock"] = (int32_t)(millis() - lockUntil) < 0;
  d["pause"] = (bool)touchPaused;
  send(d);
}

// ---------------- setup / loop ----------------
void setup() {
  relayOff();                                           // first thing: pump off
  sensorsOff();                                         // moisture sensors off and floating
  Serial.setRxBufferSize(2048);                         // the Pi sends all settings at once when it connects
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) { pinMode(PIN_STEP[i], OUTPUT); digitalWrite(PIN_STEP[i], LOW); }
  pinMode(PIN_ENDSTOP, INPUT_PULLUP);
  if (TANK_SENSOR && TANK_IS_FLOAT) pinMode(PIN_TANK, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);                       // full 0..3.3 V range
  setupLeds();
  ledSelfTest();                                        // the strip must show red, green, blue, white
  if (!HAS_ENDSTOP) { stepPos = 0; homed = true; }      // counted from where it stands now = plant 1
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
  touchSetConfig(TOUCH_CHG_TIMES, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2);   // before the first touchRead
#else
  touchSetCycles(TOUCH_MEAS_CYCLES, 0xF);               // before the first touchRead
#endif
  startTouch();                                         // calibrates 30 s in the background: don't touch the plants
  JsonDocument d;
  d["t"] = "hello";
  d["fw"] = "spicetracks-2.10";
  char core[16];
  snprintf(core, sizeof(core), "%d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  d["core"] = core;                                     // Arduino-ESP32 version, decides which touch driver runs
  d["idf"] = esp_get_idf_version();
  send(d);
}

void loop() {
  readSerial();
  uint32_t now = millis();                              // only now: a command read just above must not look older than "now"

  if (pumpOn && (now >= pumpUntil || tankEmpty())) stopWater(true);   // safety: always stops on its own

  forwardTouch();
  moistTick(now);

  if (now - lastStatusMs >= STATUS_EVERY_MS) {
    lastStatusMs = now;
    sendStatus();
  }

  if (lastPiMs && (int32_t)(now - lastPiMs) > (int32_t)PI_TIMEOUT_MS && mode != MODE_IDLE) {
    mode = MODE_IDLE;                                   // Pi gone: calm light, nothing else
    activePlant = -1;
  }

  static uint32_t lastMon = 0;
  if (touchMonUntil && (int32_t)(now - touchMonUntil) < 0 && now - lastMon >= 200) {
    lastMon = now;
    sendTouchMon();
  }

  static uint32_t lastFrame = 0;
  if (now - lastFrame >= 16) {                          // about 60 fps
    lastFrame = now;
    renderLeds();
  }
}
