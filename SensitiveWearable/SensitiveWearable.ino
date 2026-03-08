// take a breath - wearable project
// cpx + pressure sensor + ldr + mic
// digf 20166 p2

#include <Adafruit_CircuitPlayground.h>
#include <math.h>

// pins
const int PRESSURE_PIN = A4;
const int LDR_PIN = A0; // shared w speaker, read every 2s only

// led stuff
const int NUM_PIXELS = 10;

// pressure sensor - auto calibrate on boot
const int PRESSURE_OFFSET = 150; // lower for thru fabric
const int PRESSURE_HYS = 20;
int pressBase = 0;
int pressThresh = 200;

// mic - peak hold
const int SOUND_NOISY = 25; // need real loud not just quiet noise

// accel - movement detect
const float MOTION_THRESH = 2.0; // bit lower for wearing on body
const float ACCEL_SMOOTH = 0.15;

// timers
const unsigned long SIT_TIME = 30000; // 30s
const float PRESS_SMOOTH = 0.5;

// breathing speed
const unsigned long BREATH_CALM = 6000;  // 6s light green
const unsigned long BREATH_DEEP = 10000; // 10s deep blue (4s in, 6s out)

// beep settings (only when press + loud)
const int BEEP_HI = 523;
const int BEEP_LO = 392;
const int BEEP_LEN = 200;

// ldr brightness ctrl
const int LDR_DARK = 200;
const int LDR_BRIGHT = 900;
const int BRIGHT_DARK = 10;  // dark room = brighter led
const int BRIGHT_LIGHT = 0;  // strong light = off completely

// 4 modes
enum Mode {
  MODE_RAINBOW,    // sit too long
  MODE_GREEN,      // calm breathing
  MODE_DEEPBLUE,   // pressing = stress
  MODE_ORANGE      // moving around
};

// --- global vars ---
Mode curMode = MODE_GREEN;
unsigned long modeStart = 0;

float smoothPress = 0;
int sndPeak = 0;

float lastAccel = 0;
float smoothDelta = 0;
unsigned long lastMoveTime = 0;
unsigned long lastActiveTime = 0;
bool isMoving = false;
const unsigned long MOVE_LINGER = 2000; // stay orange 2s after stop

bool picking = false;
unsigned long lastPickTime = 0;
const unsigned long PICK_LINGER = 500; // 0.5s delay b4 exit blue

int serialCnt = 0;
unsigned long lastBeep = 0;
int ldrVal = 500;
unsigned long lastLdrRead = 0;

// unused rainbow palette kept just in case
const uint8_t COLORS[][3] = {
  {255, 40, 80}, {255, 100, 0}, {255, 200, 0},
  {50, 255, 50}, {0, 220, 160}, {0, 120, 255},
  {80, 40, 255}, {180, 0, 255}, {255, 0, 180}, {255, 60, 120}
};


void setup() {
  Serial.begin(9600);
  CircuitPlayground.begin();
  CircuitPlayground.setBrightness(BRIGHT_DARK);
  CircuitPlayground.clearPixels();

  // read accel once
  float x = CircuitPlayground.motionX();
  float y = CircuitPlayground.motionY();
  float z = CircuitPlayground.motionZ();
  lastAccel = sqrt(x*x + y*y + z*z);
  lastMoveTime = millis();

  // calibrate pressure - dont touch sensor!!
  Serial.println(F("calibrating... dont touch sensor"));
  for (int i = 0; i < NUM_PIXELS; i++)
    CircuitPlayground.setPixelColor(i, 15, 15, 15); // white = calibrating

  long sum = 0;
  for (int n = 0; n < 20; n++) {
    sum += analogRead(PRESSURE_PIN);
    delay(50);
  }
  pressBase = (int)(sum / 20);
  pressThresh = pressBase + PRESSURE_OFFSET;
  smoothPress = pressBase;

  CircuitPlayground.clearPixels();
  Serial.print(F("base=")); Serial.print(pressBase);
  Serial.print(F(" thresh=")); Serial.println(pressThresh);
  Serial.println(F("P_raw\tP_smo\tSndPk\tMoving\tPicking\tNoisy\tMode\tLDR\tBright"));
}


void loop() {
  unsigned long now = millis();

  // read pressure
  int rawPress = analogRead(PRESSURE_PIN);

  // read mic 10x, keep max (catch loud sounds)
  int sndMax = 0;
  for (int s = 0; s < 10; s++) {
    int val = CircuitPlayground.soundSensor();
    if (val > sndMax) sndMax = val;
  }
  // peak hold - decay slow
  if (sndMax > sndPeak) {
    sndPeak = sndMax;
  } else {
    sndPeak = sndPeak - 1;
    if (sndPeak < 0) sndPeak = 0;
  }

  smoothPress = (PRESS_SMOOTH * rawPress) + ((1.0 - PRESS_SMOOTH) * smoothPress);

  // detect pressing (0.5s linger so it dont flicker)
  bool pressedNow = (smoothPress > (pressThresh + PRESSURE_HYS));
  bool releasedNow = (smoothPress < (pressThresh - PRESSURE_HYS));

  if (pressedNow) {
    picking = true;
    lastPickTime = now;
  }
  else if (releasedNow && (now - lastPickTime) > PICK_LINGER) {
    if (picking) {
      picking = false;
      lastActiveTime = now; // reset sit timer
    }
  }

  // detect motion - skip while pressing (shake = false trigger)
  float ax = CircuitPlayground.motionX();
  float ay = CircuitPlayground.motionY();
  float az = CircuitPlayground.motionZ();
  float mag = sqrt(ax*ax + ay*ay + az*az);
  float delta = fabs(mag - lastAccel);
  lastAccel = mag;

  smoothDelta = (ACCEL_SMOOTH * delta) + ((1.0 - ACCEL_SMOOTH) * smoothDelta);

  if (smoothDelta > MOTION_THRESH && !picking) {
    lastMoveTime = now;
    lastActiveTime = now;
  }

  isMoving = (now - lastMoveTime) < MOVE_LINGER;

  // check how long sitting
  unsigned long lastAct = (lastActiveTime > lastMoveTime) ? lastActiveTime : lastMoveTime;
  bool sitTooLong = ((now - lastAct) > SIT_TIME);

  bool loud = (sndPeak > SOUND_NOISY);

  // pick mode - pressing > moving > calm > rainbow
  Mode newMode;
  if (picking) {
    newMode = MODE_DEEPBLUE; // stress detected
  }
  else if (isMoving) {
    newMode = MODE_ORANGE;
  }
  else if (!sitTooLong) {
    newMode = MODE_GREEN; // calm
  }
  else {
    newMode = MODE_RAINBOW; // move reminder
  }

  // mode changed
  if (newMode != curMode) {
    curMode = newMode;
    modeStart = now;
    printMode(newMode);

    // beep when enter blue + noisy
    if (newMode == MODE_DEEPBLUE && loud) {
      CircuitPlayground.speaker.enable(true);
      CircuitPlayground.playTone(BEEP_HI, 100);
      lastBeep = now;
    }
  }

  // keep beeping every 3s if blue + noisy
  if (curMode == MODE_DEEPBLUE && loud && (now - lastBeep) > 3000) {
    CircuitPlayground.speaker.enable(true);
    int freq = ((now / 3000) % 2 == 0) ? BEEP_HI : BEEP_LO;
    CircuitPlayground.playTone(freq, BEEP_LEN);
    lastBeep = now;
  }

  // ldr brightness - read every 2s (A0 shared w speaker)
  int bright;
  if ((now - lastLdrRead) > 2000) {
    ldrVal = analogRead(LDR_PIN);
    lastLdrRead = now;
    pinMode(A0, OUTPUT); // give A0 back to speaker
  }

  if (ldrVal < LDR_DARK) {
    // super dark = white light
    bright = BRIGHT_DARK;
    CircuitPlayground.setBrightness(bright);
    for (int i = 0; i < NUM_PIXELS; i++)
      CircuitPlayground.setPixelColor(i, 255, 255, 255);
  }
  else {
    bright = map(ldrVal, LDR_DARK, LDR_BRIGHT, BRIGHT_DARK, BRIGHT_LIGHT);
    bright = constrain(bright, BRIGHT_LIGHT, BRIGHT_DARK);

    if (bright == 0) {
      // too bright outside, turn off leds
      CircuitPlayground.clearPixels();
    }
    else {
      CircuitPlayground.setBrightness(bright);

      // show the right color mode
      switch (curMode) {
        case MODE_ORANGE: showOrange(); break;
        case MODE_DEEPBLUE: showDeepBlue(now, loud); break;
        case MODE_GREEN: showGreen(now); break;
        case MODE_RAINBOW: showRainbow(now); break;
      }
    }
  }

  // serial monitor debug
  serialCnt++;
  if (serialCnt % 40 == 0) {
    Serial.println(F("P_raw\tP_smo\tSndPk\tMoving\tPicking\tNoisy\tMode\tLDR\tBright"));
  }
  Serial.print(rawPress); Serial.print('\t');
  Serial.print((int)smoothPress); Serial.print('\t');
  Serial.print(sndPeak); Serial.print('\t');
  Serial.print(isMoving ? 1 : 0); Serial.print('\t');
  Serial.print(picking ? 1 : 0); Serial.print('\t');
  Serial.print(loud ? 1 : 0); Serial.print('\t');
  Serial.print(curMode); Serial.print('\t');
  Serial.print(ldrVal); Serial.print('\t');
  Serial.println(bright);

  delay(50);
}


// orange = moving!
void showOrange() {
  for (int i = 0; i < NUM_PIXELS; i++)
    CircuitPlayground.setPixelColor(i, 255, 60, 0);
}

// deep blue breathing - 4s inhale 6s exhale
void showDeepBlue(unsigned long now, bool loud) {
  unsigned long t = (now - modeStart) % BREATH_DEEP;
  float wave;

  if (t < 4000) {
    wave = (float)t / 4000.0; // inhale
  } else {
    wave = 1.0 - ((float)(t - 4000) / 6000.0); // exhale
  }

  // smooth it
  wave = (sin((wave - 0.5) * PI) + 1.0) / 2.0;

  float minB = 0.25; // never fully off
  float br = minB + wave * (1.0 - minB);

  uint8_t r = (uint8_t)(10 * br);
  uint8_t g = (uint8_t)(30 * br);
  uint8_t b = (uint8_t)(255 * br);

  for (int i = 0; i < NUM_PIXELS; i++)
    CircuitPlayground.setPixelColor(i, r, g, b);
}

// calm green breathing - 6s cycle
void showGreen(unsigned long now) {
  float phase = (float)(now % BREATH_CALM) / (float)BREATH_CALM;
  float wave = (sin(phase * 2.0 * PI) + 1.0) / 2.0;

  float minB = 0.25;
  float br = minB + wave * (1.0 - minB);

  uint8_t r = (uint8_t)(30 * br);
  uint8_t g = (uint8_t)(220 * br);
  uint8_t b = (uint8_t)(80 * br);

  for (int i = 0; i < NUM_PIXELS; i++)
    CircuitPlayground.setPixelColor(i, r, g, b);
}

// hsv helper for rainbow
void hsv2rgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
  int hi = (int)(h / 60.0) % 6;
  float f = h / 60.0 - (int)(h / 60.0);
  float p = v * (1.0 - s);
  float q = v * (1.0 - f * s);
  float t = v * (1.0 - (1.0 - f) * s);
  float rr, gg, bb;
  switch (hi) {
    case 0: rr=v; gg=t; bb=p; break;
    case 1: rr=q; gg=v; bb=p; break;
    case 2: rr=p; gg=v; bb=t; break;
    case 3: rr=p; gg=q; bb=v; break;
    case 4: rr=t; gg=p; bb=v; break;
    default: rr=v; gg=p; bb=q; break;
  }
  r = (uint8_t)(rr * 255);
  g = (uint8_t)(gg * 255);
  b = (uint8_t)(bb * 255);
}

// rainbow spinning - sit too long reminder
void showRainbow(unsigned long now) {
  float rot = (float)(now % 4000) / 4000.0 * 360.0;
  float pulse = (sin((float)(now % 4000) / 4000.0 * 2.0 * PI) + 1.0) / 2.0;
  float br = 0.20 + pulse * 0.30;

  for (int i = 0; i < NUM_PIXELS; i++) {
    float hue = fmod(rot + (float)i * 36.0, 360.0);
    uint8_t r, g, b;
    hsv2rgb(hue, 1.0, br, r, g, b);
    CircuitPlayground.setPixelColor(i, r, g, b);
  }
}

// print what mode changed to
void printMode(Mode m) {
  Serial.print(F(">> "));
  switch (m) {
    case MODE_ORANGE: Serial.println(F("ORANGE - moving")); break;
    case MODE_DEEPBLUE: Serial.println(F("DEEP BLUE - take a breath")); break;
    case MODE_GREEN: Serial.println(F("GREEN - calm")); break;
    case MODE_RAINBOW: Serial.println(F("RAINBOW - go move!")); break;
  }
}
