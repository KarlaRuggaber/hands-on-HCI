// Spicetracks Pflanzen-Touch v4 (Lukas), eingebaut in die Spicetracks-Firmware.
//
// Messung, Kalibrierung, Schwellen und Entprellung sind unverändert aus capacitive_touch.ino.
// Geändert ist nur der Rahmen, damit es neben LEDs, Pumpe und Pi-Verbindung läuft:
//  - läuft als eigener Task auf Kern 0 (die Messfenster blockieren 100 ms, Kern 1 macht LEDs und Serial)
//  - alle vier Pins, PLOT aus
//  - Ausgaben gehen über eine Warteschlange an loop(), damit sich keine Zeilen mit dem Rest mischen
//  - Pflanzen im JSON 0..3 statt 1..4, so wie der Pi sie zählt
//  - zwischen zwei Messfenstern 1 ms Pause (vTaskDelay), sonst schlägt der Watchdog von Kern 0 an
//  - Neukalibrierung kommt vom Pi ({"cmd":"touchcal"}) statt über 'c' im seriellen Monitor
//
// Neu am 30.10. (Feuchtesensor-Problem):
//  - Ein eingeschalteter Feuchtesensor im Topf lässt die Touch-Messung abbrechen (Timeout). Der Chip hält dann
//    ALLE Kanäle an. Darum: Feuchtesensoren nur kurz an (siehe .ino), Touch pausiert in dieser Zeit (touchPauseReq).
//  - Hängt trotzdem ein Kanal (Wert über 2 s exakt gleich), startet measureWindow() die Messung neu,
//    der Kanal meldet "stuck" und löst keine Berührungen aus. Die anderen Pflanzen laufen weiter.
#pragma once
#include <algorithm>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 5, 0)
#include "driver/touch_sensor.h"                  // alter Touch-Treiber (Core 3.2.x): touch_pad_timeout_resume()
#define TOUCH_RESUME() touch_pad_timeout_resume()
#else
#define TOUCH_RESUME() do {} while (0)
#endif

const int  PINS[] = {11, 12, 13, 14};
const int  NCH    = sizeof(PINS) / sizeof(PINS[0]);
const bool PLOT   = false;                // im Gesamtsystem immer false

// Einstellungen
const int      SAMPLES      = 100;    // 100 Messungen ...
const int      SAMPLE_US    = 1000;   // ... im Abstand von 1 ms = 100-ms-Fenster (5 Netzperioden)
const int      CAL_WINDOWS  = 300;    // 300 Fenster = 30 s Kalibrierung
const float    K_ON         = 6.0;    // Berührung ab K_ON x Rauschen der Kalibrierung
const float    MIN_THR      = 6;      // Untergrenze der Schwelle in Zählern
const bool     USE_HUM      = false;  // true: auch stärkeres Brummen zählt als Berührung
const int      ON_FRAMES    = 2;      // 2 Fenster = 0,2 s über der Schwelle
const int      OFF_FRAMES   = 3;      // 3 Fenster = 0,3 s darunter
const uint32_t STEP_MS      = 4000;   // erhöht, aber ohne Brummen: Umgebung, neu übernehmen
const uint32_t MAX_TOUCH_MS = 15000;  // länger berührt ist unrealistisch: neu übernehmen
const uint32_t WATER_LOCK   = 60000;  // Sperre nach dem Giessen
const int      STUCK_FRAMES = 20;     // 20 Fenster = 2 s ohne jede Änderung: Messung hängt
const int      SETTLE_FRAMES = 3;     // nach einer Pause 0,3 s messen, aber nicht auswerten

struct Channel {
  int pin;
  float base, thrOn, thrOff, humIdle, humThr;
  bool touched; int onCnt, offCnt;
  uint32_t since, calmSince;
  float mean, hum, delta;
  float lastMean; bool flat, stuck; int stuckCnt;   // hängt die Messung?
  bool calBad;                                       // hing während der Kalibrierung: Schwellen ungültig
};
Channel ch[NCH];
volatile uint32_t lockUntil = 0;
volatile bool needRecal = false;
volatile bool touchCalibrating = true;
volatile int  pendingPins[NCH];          // neue Zuordnung Pin -> Pflanze vom Pi ({"cmd":"touchmap"})
volatile bool pinsPending = false;
volatile bool touchPauseReq = false;     // loop(): gleich gehen die Feuchtesensoren an
volatile bool touchPaused = false;       // Antwort des Touch-Tasks: misst gerade nicht
int settleLeft = 0;

// ---- Ausgabe über eine Warteschlange an loop() ----
QueueHandle_t touchQ;
struct TouchLine { char s[120]; };

void touchOut(const char *fmt, ...) {
  TouchLine l;
  va_list ap; va_start(ap, fmt); vsnprintf(l.s, sizeof(l.s), fmt, ap); va_end(ap);
  xQueueSend(touchQ, &l, 0);            // Warteschlange voll: Zeile fällt weg, die Messung läuft weiter
}

void emit(int i, const char *type, uint32_t ms) {
  if (!PLOT) touchOut("{\"t\":\"%s\",\"plant\":%d,\"ms\":%lu}", type, i, (unsigned long)ms);
}

float medianOf(float *a, int n) {
  std::sort(a, a + n);
  return (n % 2) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

// 100 ms lang jede Millisekunde messen: Mittelwert (ohne Brummen) und Brummstärke
void measureWindow() {
  static float buf[NCH][SAMPLES];
  uint32_t t = micros();
  for (int s = 0; s < SAMPLES; s++) {
    for (int i = 0; i < NCH; i++) buf[i][s] = touchRead(ch[i].pin);
    t += SAMPLE_US;
    while ((int32_t)(micros() - t) < 0) {}
  }
  bool anyFlat = false;
  for (int i = 0; i < NCH; i++) {
    float sum = 0;
    for (int s = 0; s < SAMPLES; s++) sum += buf[i][s];
    float m = sum / SAMPLES, dev = 0;
    for (int s = 0; s < SAMPLES; s++) dev += fabsf(buf[i][s] - m);
    ch[i].lastMean = ch[i].mean;
    ch[i].mean = m;
    ch[i].hum  = dev / SAMPLES;
    ch[i].flat = dev == 0 && m == ch[i].lastMean;   // ein ganzes Fenster ohne jede Änderung
    anyFlat |= ch[i].flat;
  }
  if (anyFlat) TOUCH_RESUME();                      // Messung abgebrochen: weiter mit dem nächsten Kanal
}

// 30 s messen, ohne zu berühren. Danach stehen Ruhewert und Schwellen fest.
void calibrateAll() {
  static float m[NCH][CAL_WINDOWS], h[NCH][CAL_WINDOWS];
  static float tmp[CAL_WINDOWS];
  int flatCnt[NCH] = {0};
  touchCalibrating = true;
  for (int w = 0; w < CAL_WINDOWS; w++) {
    measureWindow();
    for (int i = 0; i < NCH; i++) { m[i][w] = ch[i].mean; h[i][w] = ch[i].hum; flatCnt[i] += ch[i].flat; }
#ifdef RGB_BUILTIN
    rgbLedWrite(RGB_BUILTIN, 0, 0, ((w / 5) % 2) ? 30 : 0);   // blau blinken = Kalibrierung läuft
#endif
    if (w % 10 == 0) touchOut("Kalibrierung_noch_s:%d", (CAL_WINDOWS - w) / 10);
    vTaskDelay(1);
  }
  for (int i = 0; i < NCH; i++) {
    Channel &c = ch[i];
    memcpy(tmp, m[i], sizeof(tmp));
    c.base = medianOf(tmp, CAL_WINDOWS);
    for (int w = 0; w < CAL_WINDOWS; w++) tmp[w] = fabsf(m[i][w] - c.base);
    float sigma = 1.4826f * medianOf(tmp, CAL_WINDOWS);      // robustes Rauschmass
    c.thrOn  = max(MIN_THR, K_ON * sigma);
    c.thrOff = c.thrOn * 0.5f;

    memcpy(tmp, h[i], sizeof(tmp));
    c.humIdle = medianOf(tmp, CAL_WINDOWS);
    for (int w = 0; w < CAL_WINDOWS; w++) tmp[w] = fabsf(h[i][w] - c.humIdle);
    float hs = 1.4826f * medianOf(tmp, CAL_WINDOWS);
    c.humThr = c.humIdle + max(2.0f, K_ON * hs);

    c.touched = false; c.onCnt = c.offCnt = 0; c.calmSince = 0;
    c.calBad = flatCnt[i] > CAL_WINDOWS / 2;             // meist gehangen: nach dem Erholen neu kalibrieren
    if (!PLOT) touchOut("{\"t\":\"cal\",\"plant\":%d,\"base\":%.1f,\"thr\":%.1f,\"hum\":%.1f}",
                        i, c.base, c.thrOn, c.humIdle);
  }
#ifdef RGB_BUILTIN
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
#endif
  touchCalibrating = false;
}

void afterWatering() { lockUntil = millis() + WATER_LOCK; needRecal = true; }   // vom Pumpen-Code aufrufen

void process(int i, uint32_t now) {
  Channel &c = ch[i];
  // hängt der Kanal? Dann keine Berührungen auswerten, die anderen Kanäle laufen normal weiter
  c.stuckCnt = c.flat ? min(c.stuckCnt + 1, STUCK_FRAMES) : 0;
  bool wasStuck = c.stuck;
  c.stuck = c.stuckCnt >= STUCK_FRAMES;
  if (c.stuck) {
    if (!wasStuck) touchOut("{\"t\":\"stuck\",\"plant\":%d}", i);
    if (c.touched) emit(i, "release", now - c.since);
    c.touched = false; c.onCnt = c.offCnt = 0; c.calmSince = 0;
    return;
  }
  if (wasStuck) {                                    // läuft wieder: Ruhewert neu übernehmen
    c.base = c.mean;
    if (c.calBad) needRecal = true;                  // Schwellen stammen aus der Zeit, als er hing
    touchOut("{\"t\":\"unstuck\",\"plant\":%d}", i);
  }
  float delta = c.mean - c.base;
  c.delta = delta;
  bool humHigh = c.hum > c.humThr;
  bool on  = delta > c.thrOn  || (USE_HUM && humHigh);
  bool off = delta < c.thrOff && !(USE_HUM && humHigh);

  // Zustand mit Entprellung und Hysterese
  if (!c.touched) {
    c.onCnt = on ? c.onCnt + 1 : 0;
    if (c.onCnt >= ON_FRAMES) { c.touched = true; c.since = now; c.offCnt = 0; emit(i, "touch", 0); }
  } else {
    c.offCnt = off ? c.offCnt + 1 : 0;
    if (c.offCnt >= OFF_FRAMES) { c.touched = false; c.onCnt = 0; emit(i, "release", now - c.since); }
  }

  // Ruhewert nachführen, die Schwellen bleiben fest
  if (!c.touched) {
    if (delta < -c.thrOff)     c.base += (c.mean - c.base) * 0.3f;    // Ruhewert zu hoch: schnell runter
    else if (delta < c.thrOff) c.base += (c.mean - c.base) * 0.01f;   // normal, ca. 10 s
    else                       c.base += (c.mean - c.base) * 0.002f;  // Graubereich: langsam, nie festhängen
  }

  // Stufe: erhöht, aber ohne zusätzliches Brummen -> Umgebung, keine Hand
  if (delta > c.thrOff && !humHigh) { if (!c.calmSince) c.calmSince = now; }
  else c.calmSince = 0;
  bool step  = c.calmSince && now - c.calmSince > STEP_MS;
  bool stuck = c.touched && now - c.since > MAX_TOUCH_MS;
  if (step || stuck) {
    if (c.touched) emit(i, "release", now - c.since);
    c.base = c.mean; c.touched = false; c.onCnt = c.offCnt = 0; c.calmSince = 0;
    emit(i, "rebase", 0);
  }
}

// entspricht setup() von capacitive_touch.ino (ohne Serial.begin)
void touchSetupLukas() {
  vTaskDelay(pdMS_TO_TICKS(500));
  for (int i = 0; i < NCH; i++) {
    ch[i] = Channel(); ch[i].pin = PINS[i];
    for (int k = 0; k < 50; k++) { touchRead(PINS[i]); vTaskDelay(pdMS_TO_TICKS(5)); }   // Aufwärmen
  }
  calibrateAll();
}

// entspricht loop() von capacitive_touch.ino
void touchLoopLukas() {
  if (touchPauseReq) {                                               // Feuchtesensoren sind an: nicht messen
    touchPaused = true;
    settleLeft = SETTLE_FRAMES;
    vTaskDelay(pdMS_TO_TICKS(20));
    return;
  }
  touchPaused = false;
  if (pinsPending) {                                                 // andere Schraube pro Pflanze: neu kalibrieren
    for (int i = 0; i < NCH; i++) { ch[i].pin = pendingPins[i]; ch[i].stuck = false; ch[i].stuckCnt = 0; }
    pinsPending = false;
    needRecal = true;
  }
  if ((int32_t)(millis() - lockUntil) < 0) { vTaskDelay(pdMS_TO_TICKS(50)); return; }   // Giess-Sperre
  if (needRecal) { calibrateAll(); needRecal = false; }

  measureWindow();                                                   // dauert 100 ms
  if (settleLeft > 0) { settleLeft--; vTaskDelay(1); return; }       // nach der Pause erst einschwingen
  uint32_t now = millis();
  for (int i = 0; i < NCH; i++) process(i, now);
#ifdef RGB_BUILTIN
  bool any = false;
  for (int i = 0; i < NCH; i++) any |= ch[i].touched;
  rgbLedWrite(RGB_BUILTIN, 0, any ? 40 : 0, 0);
#endif
  vTaskDelay(1);
}

void touchTask(void *) {
  touchSetupLukas();
  for (;;) touchLoopLukas();
}

// Zustand eines Kanals für den Pi: ok, cal, pause, lock, stuck
const char *touchStateOf(int i) {
  if (touchCalibrating) return "cal";
  if (touchPaused || touchPauseReq) return "pause";
  if ((int32_t)(millis() - lockUntil) < 0 || needRecal) return "lock";
  if (ch[i].stuck) return "stuck";
  return "ok";
}

// in setup() aufrufen
void startTouch() {
  touchQ = xQueueCreate(16, sizeof(TouchLine));
  xTaskCreatePinnedToCore(touchTask, "touch", 8192, nullptr, 1, nullptr, 0);
}

// in loop() aufrufen: fertige Zeilen an den Pi weitergeben
void greet(int plant);                              // LED greeting, in the main sketch

void forwardTouch() {
  TouchLine l;
  while (xQueueReceive(touchQ, &l, 0) == pdTRUE) {
    if (!strncmp(l.s, "{\"t\":\"touch\",\"plant\":", 21)) greet(l.s[21] - '0');
    Serial.print(l.s);
    Serial.print('\n');
  }
}
