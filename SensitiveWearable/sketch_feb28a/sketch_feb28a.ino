// ============================================================
// SensitiveWearable.ino — "Take a Breath / Gentle Reminder"
// For Adafruit Circuit Playground Express (CPX)
// DIGF-20166 Project 2
// ============================================================
//
// OVERVIEW:
//   This wearable uses a GRADIENT COLOR system based on pick count:
//     0 picks  = 🔵 calm blue breathing
//     1 pick   = 🟡 yellow breathing
//     2 picks  = 🟠 orange breathing
//     3+ picks = 🔴 red breathing + gentle tone
//   Plus:
//     Noise boost: if environment is noisy, color shifts +1 step warmer
//     Sedentary:   if no movement for 20s (test) / 2min (final),
//                  green spinning dot overlay
//
// HARDWARE WIRING:
//   DIY Pressure Sensor voltage divider:
//     CPX 3.3V → Rvar (pressure patch) → Vout → Rfixed → GND
//                                          └──→ CPX A2
//
// POWER:
//   3× Ni-MH AA (1.2V each ≈ 3.6V) via JST battery connector
// ============================================================

#include <Adafruit_CircuitPlayground.h>
#include <math.h>

// ─────────────────────────────────────────────
// DEBUG MODE — forces obvious solid colors per pick count
// 0=blue, 1=yellow, 2=orange, 3+=red, sedentary=green
// Set to false for final smooth breathing animations.
// ─────────────────────────────────────────────
const bool DEBUG_MODE = false;

// ─────────────────────────────────────────────
// PIN CONFIGURATION
// ─────────────────────────────────────────────
const int PRESSURE_PIN = A4;   // Analog pin for DIY pressure sensor (was A2)

// ─────────────────────────────────────────────
// NEOPIXEL SETTINGS
// ─────────────────────────────────────────────
const int   NUM_PIXELS     = 10;
const int   MAX_BRIGHTNESS = 10;   // Lowered for softer output (was 40)

// ─────────────────────────────────────────────
// PRESSURE SENSOR THRESHOLDS
// ─────────────────────────────────────────────
// These are set AUTOMATICALLY at boot by calibration.
// The system reads the "resting" baseline, then sets:
//   threshold = baseline + PRESSURE_OFFSET
// So it adapts to whatever pin/wiring you use.
const int   PRESSURE_OFFSET     = 60;    // How far above baseline to trigger a pick
const int   PRESSURE_HYSTERESIS = 8;     // Prevents flickering near threshold

// These will be set in setup() by auto-calibration:
int pressureBaseline  = 0;   // Resting value (no press)
int pressureThreshold = 200; // Will be overwritten = baseline + OFFSET

// ─────────────────────────────────────────────
// PICK EVENT SLIDING WINDOW
// ─────────────────────────────────────────────
// Picks expire after this time, so color naturally fades back to blue
const unsigned long PICK_WINDOW_MS = 15000; // 15-second window

// ─────────────────────────────────────────────
// SOUND (MIC) — DJ METER SETTINGS
// ─────────────────────────────────────────────
// Maps sound level to 0-10 purple overlay LEDs (like a VU meter).
// SOUND_MIN = quiet room baseline (0 purple LEDs)
// SOUND_MAX = loud environment (all 10 LEDs purple)
// Tune these using Serial Monitor S_smo column.
const int SOUND_MIN = 10;    // Below this = silence, 0 purple LEDs (was 50)
const int SOUND_MAX = 150;   // At or above this = all 10 purple LEDs (was 500)

// Red color for the DJ meter overlay (sound warning)
const uint8_t PURPLE_R = 255;
const uint8_t PURPLE_G =   0;
const uint8_t PURPLE_B =   0;

// ─────────────────────────────────────────────
// ACCELEROMETER / SEDENTARY DETECTION
// ─────────────────────────────────────────────
const float MOTION_THRESHOLD       = 1.5;
const float SMOOTH_ALPHA_ACCEL     = 0.15;
const unsigned long SEDENTARY_TIME_MS = 20000; // 20s for testing (120000 = 2min final)

// ─────────────────────────────────────────────
// SMOOTHING
// ─────────────────────────────────────────────
const float SMOOTH_ALPHA_PRESSURE = 0.5;
const float SMOOTH_ALPHA_SOUND    = 0.3;

// ─────────────────────────────────────────────
// TONE
// ─────────────────────────────────────────────
const bool ENABLE_TONE       = true;
const int  TONE_FREQ         = 440;   // Hz
const int  TONE_DURATION_MS  = 60;    // Very quick chirp (was 120)

// ─────────────────────────────────────────────
// BREATHING ANIMATION
// ─────────────────────────────────────────────
// Higher pick count = faster breathing cycle (more urgency)
const unsigned long BREATH_CYCLE_0 = 6000;  // 6s for 0 picks (very calm)
const unsigned long BREATH_CYCLE_1 = 5000;  // 5s for 1 pick
const unsigned long BREATH_CYCLE_2 = 4000;  // 4s for 2 picks
const unsigned long BREATH_CYCLE_3 = 3000;  // 3s for 3+ picks (guided breathing)

// ============================================================
// COLOR PALETTE — blue → yellow → orange → red gradient
// Each entry is {R, G, B} at full brightness
// ============================================================
const uint8_t COLORS[][3] = {
  {  60, 200, 120},  // Level 0: soft mint green (calm)
  { 255, 220,   0},  // Level 1: warm yellow
  { 255, 120,   0},  // Level 2: orange
  { 255,  20,   0},  // Level 3: red (urgent but still warm)
};
const int MAX_COLOR_LEVEL = 3;

// ============================================================
// GLOBAL STATE
// ============================================================

// — Smoothed sensor values —
float smoothPressure = 0;
float smoothSound    = 0;

// — Pressure pick tracking —
unsigned long pickTimestamps[20];  // Circular buffer
int pickIndex   = 0;
int pickCount   = 0;
bool pickActive = false;

// — Accelerometer —
float lastAccelMag       = 0;
float smoothedAccelDelta = 0;
unsigned long lastMotionTime = 0;

// — Serial —
int serialLineCount = 0;

// — Tone cooldown (avoid buzzing every loop) —
unsigned long lastToneTime = 0;

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(9600);
  CircuitPlayground.begin();
  CircuitPlayground.setBrightness(MAX_BRIGHTNESS);
  CircuitPlayground.clearPixels();

  // Init accelerometer baseline
  float x = CircuitPlayground.motionX();
  float y = CircuitPlayground.motionY();
  float z = CircuitPlayground.motionZ();
  lastAccelMag   = sqrt(x * x + y * y + z * z);
  lastMotionTime = millis();

  for (int i = 0; i < 20; i++) pickTimestamps[i] = 0;

  Serial.println(F("=== Sensitive Wearable Started ==="));

  // ── AUTO-CALIBRATE PRESSURE SENSOR ──────────────────────
  // Read the sensor 20 times over 1 second to find the resting baseline.
  // DON'T TOUCH the pressure sensor during startup!
  Serial.println(F("Calibrating pressure sensor... DON'T TOUCH!"));
  // Flash all pixels white briefly to show calibration is happening
  for (int i = 0; i < NUM_PIXELS; i++)
    CircuitPlayground.setPixelColor(i, 20, 20, 20);

  long sum = 0;
  for (int n = 0; n < 20; n++) {
    sum += analogRead(PRESSURE_PIN);
    delay(50); // 20 samples x 50ms = 1 second
  }
  pressureBaseline  = (int)(sum / 20);
  pressureThreshold = pressureBaseline + PRESSURE_OFFSET;

  // Initialize smoothPressure to baseline so it doesn't false-trigger
  smoothPressure = pressureBaseline;

  CircuitPlayground.clearPixels();
  Serial.print(F("Baseline = ")); Serial.print(pressureBaseline);
  Serial.print(F("  Threshold = ")); Serial.println(pressureThreshold);
  Serial.println(F("Calibration done! Ready."));

  if (DEBUG_MODE) {
    Serial.println(F(">>> DEBUG MODE: solid colors per level <<<"));
  }
  Serial.println(F("\nP_raw\tP_smo\tS_raw\tS_smo\tPicks\tLevel\tSeden\tpickAct\tAccDlt\tPurple"));
}

// ============================================================
// MAIN LOOP
// ============================================================
void loop() {
  unsigned long now = millis();

  // ── 1. READ & SMOOTH SENSORS ──────────────────────────────
  int rawPressure = analogRead(PRESSURE_PIN);
  int rawSound    = CircuitPlayground.soundSensor();

  smoothPressure = (SMOOTH_ALPHA_PRESSURE * rawPressure)
                 + ((1.0 - SMOOTH_ALPHA_PRESSURE) * smoothPressure);
  smoothSound    = (SMOOTH_ALPHA_SOUND * rawSound)
                 + ((1.0 - SMOOTH_ALPHA_SOUND) * smoothSound);

  // ── 2. DETECT PRESSURE "PICK" EVENTS (edge detection) ─────
  // Rising edge: pressure crosses above threshold
  if (!pickActive && smoothPressure > (pressureThreshold + PRESSURE_HYSTERESIS)) {
    pickActive = true;
    pickTimestamps[pickIndex] = now;
    pickIndex = (pickIndex + 1) % 20;
    Serial.println(F(">> PICK detected!"));
  }
  // Falling edge: pressure drops below threshold (reset for next pick)
  if (pickActive && smoothPressure < (pressureThreshold - PRESSURE_HYSTERESIS)) {
    pickActive = false;
  }

  // Count picks within sliding window
  pickCount = 0;
  for (int i = 0; i < 20; i++) {
    if (pickTimestamps[i] != 0 && (now - pickTimestamps[i]) < PICK_WINDOW_MS) {
      pickCount++;
    }
  }

  // ── 3. DETECT SEDENTARY (ACCELEROMETER) ───────────────────
  float ax = CircuitPlayground.motionX();
  float ay = CircuitPlayground.motionY();
  float az = CircuitPlayground.motionZ();
  float accelMag = sqrt(ax * ax + ay * ay + az * az);
  float rawDelta = fabs(accelMag - lastAccelMag);
  lastAccelMag = accelMag;

  smoothedAccelDelta = (SMOOTH_ALPHA_ACCEL * rawDelta)
                     + ((1.0 - SMOOTH_ALPHA_ACCEL) * smoothedAccelDelta);

  if (smoothedAccelDelta > MOTION_THRESHOLD) {
    lastMotionTime = now;
  }

  bool isSedentary = (now - lastMotionTime) > SEDENTARY_TIME_MS;

  // ── 4. CALCULATE COLOR LEVEL ──────────────────────────────
  // Base level from pick count: 0, 1, 2, 3+
  int colorLevel = pickCount;
  if (colorLevel > MAX_COLOR_LEVEL) colorLevel = MAX_COLOR_LEVEL;

  // ── 5. PLAY TONE at level 3 (once per cycle, not spamming) ─
  if (ENABLE_TONE && colorLevel >= 3 && (now - lastToneTime) > 3000) {  // chirp every 3s (was 6s)
    CircuitPlayground.playTone(TONE_FREQ, TONE_DURATION_MS);
    lastToneTime = now;
  }

  // ── 6. RENDER BASE COLOR ──────────────────────────────────
  if (isSedentary && colorLevel == 0) {
    renderSedentary(now);
  } else if (DEBUG_MODE) {
    for (int i = 0; i < NUM_PIXELS; i++) {
      CircuitPlayground.setPixelColor(i,
        COLORS[colorLevel][0],
        COLORS[colorLevel][1],
        COLORS[colorLevel][2]);
    }
  } else {
    renderBreathing(now, colorLevel);
  }

  // ── 7. DJ SOUND METER — purple overlay ─────────────────────
  // Map smoothSound to number of purple LEDs (0–10)
  // Louder = more purple pixels, like a DJ VU meter
  int soundLevel = (int)smoothSound;
  int purpleCount = 0;
  if (soundLevel > SOUND_MIN) {
    purpleCount = map(soundLevel, SOUND_MIN, SOUND_MAX, 0, NUM_PIXELS);
    if (purpleCount > NUM_PIXELS) purpleCount = NUM_PIXELS;
    if (purpleCount < 0) purpleCount = 0;
  }

  // Overwrite the last N pixels with purple (from pixel 9 downward)
  // This way the base color is still visible on the remaining LEDs
  for (int i = 0; i < purpleCount; i++) {
    int pixelIdx = NUM_PIXELS - 1 - i;  // Fill from top down
    CircuitPlayground.setPixelColor(pixelIdx, PURPLE_R, PURPLE_G, PURPLE_B);
  }

  // ── 8. SERIAL OUTPUT ──────────────────────────────────────
  serialLineCount++;
  if (serialLineCount % 40 == 0) {
    Serial.println(F("P_raw\tP_smo\tS_raw\tS_smo\tPicks\tLevel\tSeden\tpickAct\tAccDlt\tPurple"));
  }

  Serial.print(rawPressure);      Serial.print('\t');
  Serial.print((int)smoothPressure); Serial.print('\t');
  Serial.print(rawSound);         Serial.print('\t');
  Serial.print((int)smoothSound); Serial.print('\t');
  Serial.print(pickCount);        Serial.print('\t');
  Serial.print(colorLevel);       Serial.print('\t');
  Serial.print(isSedentary ? 1 : 0); Serial.print('\t');
  Serial.print(pickActive ? 1 : 0);  Serial.print('\t');
  Serial.print(smoothedAccelDelta, 2); Serial.print('\t');
  Serial.println(purpleCount);

  delay(50);
}

// ============================================================
// RENDER: Breathing animation with color from gradient
// ============================================================
void renderBreathing(unsigned long now, int level) {
  // Higher level = faster breathing cycle
  unsigned long cycleMs;
  switch (level) {
    case 0:  cycleMs = BREATH_CYCLE_0; break;
    case 1:  cycleMs = BREATH_CYCLE_1; break;
    case 2:  cycleMs = BREATH_CYCLE_2; break;
    default: cycleMs = BREATH_CYCLE_3; break;
  }

  // Sine wave breathing: smooth 0.0 – 1.0 – 0.0
  float phase = (float)(now % cycleMs) / (float)cycleMs;
  float wave  = (sin(phase * 2.0 * PI) + 1.0) / 2.0;

  // Scale the color by the wave, with a minimum floor so it doesn't go fully off
  float minBright = 0.08;  // Always slightly visible
  float brightness = minBright + wave * (1.0 - minBright);

  // Apply brightness scaling (0.5 max to keep power draw low)
  uint8_t r = (uint8_t)(COLORS[level][0] * brightness * 0.5);
  uint8_t g = (uint8_t)(COLORS[level][1] * brightness * 0.5);
  uint8_t b = (uint8_t)(COLORS[level][2] * brightness * 0.5);

  for (int i = 0; i < NUM_PIXELS; i++) {
    CircuitPlayground.setPixelColor(i, r, g, b);
  }
}

// ============================================================
// RENDER: Sedentary — macaron pastel rainbow marquee
// Soft, dreamy rotating rainbow that gently nudges you to move.
// ============================================================

// Candy palette — saturated colors that look distinct on NeoPixels
// (Pastels look white because all RGB channels are high;
//  these have one dominant channel so each color pops)
const uint8_t MACARON[][3] = {
  { 255,  40,  80 },  // candy pink
  { 255, 100,   0 },  // tangerine
  { 255, 200,   0 },  // golden yellow
  {  50, 255,  50 },  // lime green
  {   0, 220, 160 },  // teal mint
  {   0, 120, 255 },  // ocean blue
  {  80,  40, 255 },  // indigo
  { 180,   0, 255 },  // violet
  { 255,   0, 180 },  // magenta
  { 255,  60, 120 },  // hot pink
};

void renderSedentary(unsigned long now) {
  // Slowly rotate the rainbow around the ring (10s full rotation)
  float phase = (float)(now % 10000) / 10000.0;
  int offset = (int)(phase * NUM_PIXELS);

  // Gentle pulsing brightness (subtle breathing on top of rainbow)
  float pulse = (sin((float)(now % 4000) / 4000.0 * 2.0 * PI) + 1.0) / 2.0;
  float bright = 0.10 + pulse * 0.15;  // Range 0.10 – 0.25 (dim & soft)

  for (int i = 0; i < NUM_PIXELS; i++) {
    int colorIdx = (i + offset) % NUM_PIXELS;
    CircuitPlayground.setPixelColor(i,
      (uint8_t)(MACARON[colorIdx][0] * bright),
      (uint8_t)(MACARON[colorIdx][1] * bright),
      (uint8_t)(MACARON[colorIdx][2] * bright));
  }
}
