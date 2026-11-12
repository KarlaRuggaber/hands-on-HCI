// Spicetracks Gesamttest am MacBook (ohne Pi, LEDs, Pumpen-Netzteil)
#include <algorithm>

// Pins laut Steckplan
const int  TOUCH[4] = {11, 12, 13, 14};
const int  MOIST[4] = {1, 2, 4, 5};
const int  LIGHT    = 6;
const int  RELAY    = 21;
const bool RELAY_ACTIVE_LOW = false;
const int  STEP[4]  = {15, 16, 17, 18};   // IN1 bis IN4 am ULN2003
const int  ENDSTOP  = 39;                 // nicht angeschlossen: zeigt "offen"

float tBase[4], tThr[4];

void relayWrite(bool on) { digitalWrite(RELAY, (on != RELAY_ACTIVE_LOW) ? HIGH : LOW); }

// 100 ms lang jede Millisekunde alle Touch-Pins messen (gegen Netzbrummen)
void touchWindow(float *mean) {
  float sum[4] = {0, 0, 0, 0};
  uint32_t t = micros();
  for (int s = 0; s < 100; s++) {
    for (int i = 0; i < 4; i++) sum[i] += touchRead(TOUCH[i]);
    t += 1000; while ((int32_t)(micros() - t) < 0) {}
  }
  for (int i = 0; i < 4; i++) mean[i] = sum[i] / 100;
}

void calibrateTouch() {
  Serial.println("Touch: 3 s kalibrieren, Pflanzen NICHT beruehren ...");
  for (int i = 0; i < 4; i++) for (int k = 0; k < 50; k++) touchRead(TOUCH[i]);
  const int W = 30; float m[4][W];
  for (int w = 0; w < W; w++) { float v[4]; touchWindow(v); for (int i = 0; i < 4; i++) m[i][w] = v[i]; }
  for (int i = 0; i < 4; i++) {
    float tmp[W]; memcpy(tmp, m[i], sizeof(tmp)); std::sort(tmp, tmp + W);
    tBase[i] = tmp[W / 2];
    for (int w = 0; w < W; w++) tmp[w] = fabsf(m[i][w] - tBase[i]);
    std::sort(tmp, tmp + W);
    tThr[i] = max(8.0f, 6.0f * 1.4826f * tmp[W / 2]);
    Serial.printf("  Kraut %d: Ruhewert %.0f, Schwelle +%.0f\n", i + 1, tBase[i], tThr[i]);
  }
}

float readMv(int pin) { uint32_t s = 0; for (int i = 0; i < 32; i++) s += analogReadMilliVolts(pin); return s / 32.0f; }

void lightMeasure(float &mean, float &flicker) {
  float sum = 0, lo = 1e9, hi = -1e9; uint32_t t = micros();
  for (int i = 0; i < 100; i++) {
    float mv = analogReadMilliVolts(LIGHT); sum += mv; lo = min(lo, mv); hi = max(hi, mv);
    t += 1000; while ((int32_t)(micros() - t) < 0) {}
  }
  mean = sum / 100; flicker = hi - lo;
}

void stepperMove(long steps, int dir) {
  static const uint8_t SEQ[8][4] = {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,1},{0,0,0,1},{1,0,0,1}};
  static int idx = 0;
  for (long s = 0; s < steps; s++) {
    idx = (idx + dir + 8) % 8;
    for (int c = 0; c < 4; c++) digitalWrite(STEP[c], SEQ[idx][c]);
    delay(2);
  }
  for (int c = 0; c < 4; c++) digitalWrite(STEP[c], LOW);   // Spulen aus, sonst wird der Motor heiss
}

void testRelay() {
  Serial.println("Relais: 3x an und aus. Es muss klicken, die LED am Modul leuchtet bei AN.");
  for (int i = 0; i < 3; i++) {
    relayWrite(true);  Serial.println("  AN");  delay(1000);
    relayWrite(false); Serial.println("  AUS"); delay(1000);
  }
}

void testStepper() {
  Serial.println("Motor: Viertelumdrehung vor, dann zurueck.");
  stepperMove(1024, +1); delay(300); stepperMove(1024, -1);
  Serial.println("  Fertig. Brummt er nur oder zittert: Reihenfolge IN1 bis IN4 und 5 V am ULN2003 pruefen.");
}

void testMoisture() {
  Serial.println("Feuchte: 10 Messungen. In Luft hoch, in nasser Erde deutlich tiefer.");
  for (int k = 0; k < 10; k++) {
    Serial.printf("  F1 %4.0f  F2 %4.0f  F3 %4.0f  F4 %4.0f mV\n",
                  readMv(MOIST[0]), readMv(MOIST[1]), readMv(MOIST[2]), readMv(MOIST[3]));
    delay(500);
  }
}

void testLight() {
  Serial.println("Licht: 8 s. Sensor abdecken und mit der Taschenlampe anleuchten.");
  for (int k = 0; k < 20; k++) {
    float m, f; lightMeasure(m, f);
    Serial.printf("  %4.0f mV   Flackern %4.0f mV%s\n", m, f, f > 1000 ? "   <- Pin haengt in der Luft!" : "");
    delay(300);
  }
}

void testTouch() {
  Serial.println("Touch: 15 s. Pflanzen nacheinander beruehren.");
  uint32_t end = millis() + 15000;
  while ((int32_t)(millis() - end) < 0) {
    float v[4]; touchWindow(v);
    for (int i = 0; i < 4; i++) { float d = v[i] - tBase[i]; Serial.printf("  K%d %+5.0f %s", i + 1, d, d > tThr[i] ? "[X]" : "[ ]"); }
    Serial.println();
  }
}

void testEndstop() { Serial.printf("Endschalter: %s\n", digitalRead(ENDSTOP) == LOW ? "gedrueckt" : "offen"); }

void help() {
  Serial.println("\nSpicetracks Test. Buchstabe eingeben + Enter:");
  Serial.println("  f Feuchtesensoren   l Lichtsensor   t Touch   c Touch neu kalibrieren");
  Serial.println("  r Relais            s Schrittmotor  e Endschalter");
  Serial.println("  a alles nacheinander   ? Hilfe");
}

void setup() {
  relayWrite(false); pinMode(RELAY, OUTPUT);                  // Relais zuerst sicher aus
  for (int c = 0; c < 4; c++) { pinMode(STEP[c], OUTPUT); digitalWrite(STEP[c], LOW); }
  pinMode(ENDSTOP, INPUT_PULLUP);
  Serial.begin(115200);
  delay(1500);
  calibrateTouch();
  help();
}

void loop() {
  if (!Serial.available()) return;
  char k = Serial.read();
  switch (k) {
    case 'f': testMoisture(); break;
    case 'l': testLight(); break;
    case 't': testTouch(); break;
    case 'c': calibrateTouch(); break;
    case 'r': testRelay(); break;
    case 's': testStepper(); break;
    case 'e': testEndstop(); break;
    case 'a': testMoisture(); testLight(); testEndstop(); testTouch(); testRelay(); testStepper(); break;
    case '?': help(); break;
  }
}