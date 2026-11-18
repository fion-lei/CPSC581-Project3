/*
  Eat-With-Me placemats (single-LED proof of concept)
  CPSC 581 Group Project 3 - Augmented Eating Experience

  A caregiver's bites (tilt switch in their spoon) make the diner's plate glow.
  Photoresistors under the diner's spoon rest and cup coaster log bites and sips.
  The caregiver's LCD shows counts, a "time since last bite" timer, and prompts
  ("Eat together now", "Offer a drink").

  Board: Arduino Uno R3 (ELEGOO UNO R3 Super Starter Kit)
  Library: LiquidCrystal (built into the Arduino IDE)

  Pin map
    D2   tilt ball switch (other leg to GND), INPUT_PULLUP
    D3   push button (other leg to GND), INPUT_PULLUP
    D7   LCD RS
    D8   passive buzzer
    D10  220 ohm -> plate LED anode, cathode to GND (PWM fade)
    D12  LCD E
    A0   photoresistor divider, diner spoon rest
    A1   photoresistor divider, diner cup coaster
    A2-A5 LCD D4-D7

  Button
    Short press: confirm calibration step / end meal (summary) / start new meal
    Long press (2 s): recalibrate
*/

#include <LiquidCrystal.h>

// ---------------- Pins ----------------
const uint8_t PIN_TILT      = 2;
const uint8_t PIN_BTN       = 3;
const uint8_t PIN_BUZZ      = 8;
const uint8_t PIN_PLATE_LED = 10;   // PWM
const uint8_t PIN_LDR_SPOON = A0;
const uint8_t PIN_LDR_CUP   = A1;

LiquidCrystal lcd(7, 12, A2, A3, A4, A5);  // RS, E, D4, D5, D6, D7

// ---------------- Settings ----------------
// Set DEMO_MODE to 1 for critique: nudge after 20 s, drink prompt after 30 s.
#define DEMO_MODE 1

const unsigned long BITE_HOLD_MS     = 500;   // tilt must hold this long
const unsigned long BITE_COOLDOWN_MS = 3000;  // min gap between caregiver bites
const unsigned long FADE_MS          = 1500;  // glow fade in / out
const unsigned long GLOW_HOLD_MS     = 5000;  // glow hold at full
const unsigned long LIFT_STABLE_MS   = 300;   // photoresistor change must hold
const unsigned long PULSE_PERIOD_MS  = 2000;  // one "breath" of the nudge
const uint8_t       NUDGE_PULSES     = 3;
const unsigned long DEBOUNCE_MS      = 40;
const unsigned long LONG_PRESS_MS    = 2000;
const unsigned long LCD_REFRESH_MS   = 500;
const unsigned long LINE1_SWAP_MS    = 3000;  // alternate line 1 contents

#if DEMO_MODE
const unsigned long NUDGE_MS = 20000UL;
const unsigned long SIP_MS   = 30000UL;
#else
const unsigned long NUDGE_MS = 240000UL;  // 4 min
const unsigned long SIP_MS   = 600000UL;  // 10 min
#endif

const bool CHIME_ENABLED = true;
// Flip this if your bench test shows the switch OPENS when the spoon tips to the mouth.
const bool TILT_CLOSED_MEANS_BITE = true;
const int  MIN_CAL_GAP = 100;   // covered vs open must differ by this many counts

// ---------------- State ----------------
enum State { CAL_COVERED, CAL_OPEN, CAL_ERROR, MEAL, SUMMARY };
State state = CAL_COVERED;

struct LiftSensor {
  uint8_t pin;
  int covered;
  int open;
  int threshold;
  bool up;              // debounced: true = lifted (light on sensor)
  bool candidate;
  unsigned long since;
};
LiftSensor spoon = { PIN_LDR_SPOON, 0, 0, 512, false, false, 0 };
LiftSensor cup   = { PIN_LDR_CUP,   0, 0, 512, false, false, 0 };

enum GlowMode { G_OFF, G_FADE_IN, G_HOLD, G_FADE_OUT, G_PULSE };
GlowMode glowMode = G_OFF;
unsigned long glowStart = 0;
uint8_t glowLevel = 0;
uint8_t fadeFrom = 0;

unsigned int cgBites = 0, dinerBites = 0, sips = 0;
unsigned long mealStart = 0, lastDinerBite = 0, lastSip = 0, lastNudge = 0;
unsigned long lastCgBite = 0, mealMinutes = 0;
bool prompt = false;     // "Eat together now"
bool sipDue = false;     // "Offer a drink"

bool tiltWas = false, tiltCounted = false;
unsigned long tiltSince = 0;

bool btnStable = false, btnRaw = false, longFired = false;
unsigned long btnChanged = 0, btnPressedAt = 0;

unsigned long lastLcd = 0;

// ---------------- Helpers ----------------
int averageRead(uint8_t pin) {
  long sum = 0;
  for (uint8_t i = 0; i < 20; i++) { sum += analogRead(pin); delay(5); }
  return (int)(sum / 20);
}

// Returns true once, on the debounced change from covered to lifted.
bool liftEvent(LiftSensor &s, unsigned long now) {
  bool raw = analogRead(s.pin) > s.threshold;
  if (raw != s.candidate) { s.candidate = raw; s.since = now; }
  if (s.candidate != s.up && now - s.since >= LIFT_STABLE_MS) {
    s.up = s.candidate;
    return s.up;
  }
  return false;
}

void printLine(uint8_t row, const char *text) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16s", text);   // pad to 16 so old text is overwritten
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ---------------- Melody ----------------
// Passive-buzzer melody using tone(): note frequencies in Hz, note types
// (4 = quarter, 8 = eighth) give the length as 1000 / type ms, and each note
// is followed by a gap of 30% of its length so the notes sound distinct.
// Played without delay() so sensors, glow and LCD keep running meanwhile.
#define NOTE_C5 523
#define NOTE_E5 659
#define NOTE_G5 784
#define NOTE_C6 1047
#define REST    0

const int     MELODY[]     = { NOTE_C5, NOTE_E5, NOTE_G5, REST, NOTE_E5, NOTE_C6 };
const uint8_t NOTE_TYPES[] = { 8,       8,       8,       8,    8,       4       };
const uint8_t MELODY_LEN   = sizeof(MELODY) / sizeof(MELODY[0]);

int8_t melodyIndex = -1;          // -1 = not playing
unsigned long noteStart = 0, noteSpan = 0;

void playNote(unsigned long now) {
  unsigned long len = 1000UL / NOTE_TYPES[melodyIndex];
  if (MELODY[melodyIndex] == REST) noTone(PIN_BUZZ);
  else tone(PIN_BUZZ, MELODY[melodyIndex], len);
  noteStart = now;
  noteSpan = len * 13 / 10;       // note plus 30% gap
}

void startMelody(unsigned long now) {
  if (!CHIME_ENABLED) return;
  melodyIndex = 0;
  playNote(now);
}

void updateMelody(unsigned long now) {
  if (melodyIndex < 0 || now - noteStart < noteSpan) return;
  melodyIndex++;
  if (melodyIndex >= MELODY_LEN) { noTone(PIN_BUZZ); melodyIndex = -1; }
  else playNote(now);
}

void startGlow(unsigned long now) {
  // Continue from the current brightness so the fade never jumps.
  glowStart = now - (unsigned long)glowLevel * FADE_MS / 255;
  glowMode = G_FADE_IN;
}

void stopGlow(unsigned long now) {
  if (glowMode == G_OFF) return;
  fadeFrom = glowLevel;
  glowStart = now;
  glowMode = G_FADE_OUT;
}

void startPulse(unsigned long now) {
  glowStart = now;
  glowMode = G_PULSE;
}

void updateGlow(unsigned long now) {
  unsigned long t = now - glowStart;
  switch (glowMode) {
    case G_OFF:
      glowLevel = 0;
      break;
    case G_FADE_IN:
      if (t >= FADE_MS) { glowLevel = 255; glowMode = G_HOLD; glowStart = now; }
      else glowLevel = (uint8_t)(255UL * t / FADE_MS);
      break;
    case G_HOLD:
      glowLevel = 255;
      if (t >= GLOW_HOLD_MS) { fadeFrom = 255; glowMode = G_FADE_OUT; glowStart = now; }
      break;
    case G_FADE_OUT:
      if (t >= FADE_MS) { glowLevel = 0; glowMode = G_OFF; }
      else glowLevel = (uint8_t)(fadeFrom - (unsigned long)fadeFrom * t / FADE_MS);
      break;
    case G_PULSE: {
      if (t >= (unsigned long)NUDGE_PULSES * PULSE_PERIOD_MS) { glowLevel = 0; glowMode = G_OFF; break; }
      unsigned long phase = t % PULSE_PERIOD_MS;
      unsigned long half = PULSE_PERIOD_MS / 2;
      glowLevel = (uint8_t)(phase < half ? 255UL * phase / half
                                         : 255UL * (PULSE_PERIOD_MS - phase) / half);
      break;
    }
  }
  analogWrite(PIN_PLATE_LED, glowLevel);
}

void startMeal(unsigned long now) {
  cgBites = dinerBites = sips = 0;
  mealStart = lastDinerBite = lastSip = lastNudge = now;
  lastCgBite = 0;
  prompt = false;
  sipDue = false;
  glowMode = G_OFF;
  // Start debounced sensors from their current state so a resting spoon is not a bite.
  spoon.up = spoon.candidate = analogRead(spoon.pin) > spoon.threshold;
  cup.up   = cup.candidate   = analogRead(cup.pin)   > cup.threshold;
  state = MEAL;
  Serial.println(F("MEAL start"));
}

// ---------------- Button ----------------
// Returns 1 for short press (on release), 2 for long press (while held), 0 otherwise.
uint8_t readButton(unsigned long now) {
  bool raw = digitalRead(PIN_BTN) == LOW;
  if (raw != btnRaw) { btnRaw = raw; btnChanged = now; }
  if (now - btnChanged < DEBOUNCE_MS) return 0;

  if (raw != btnStable) {
    btnStable = raw;
    if (raw) { btnPressedAt = now; longFired = false; }
    else if (!longFired) return 1;
  }
  if (btnStable && !longFired && now - btnPressedAt >= LONG_PRESS_MS) {
    longFired = true;
    return 2;
  }
  return 0;
}

// ---------------- LCD ----------------
void drawLcd(unsigned long now) {
  char l1[17], l2[17];
  switch (state) {
    case CAL_COVERED:
      printLine(0, "Place spoon+cup");
      printLine(1, "then press btn");
      return;
    case CAL_OPEN:
      printLine(0, "Lift spoon+cup");
      printLine(1, "then press btn");
      return;
    case CAL_ERROR:
      printLine(0, "Need more light");
      printLine(1, "press to retry");
      return;
    case MEAL: {
      bool firstSlot = ((now - mealStart) / LINE1_SWAP_MS) % 2 == 0;
      // Line 1 alternates every LINE1_SWAP_MS. Prompts take over their slot,
      // and when both are active they take turns so neither hides the other.
      if (firstSlot) {
        if (prompt) snprintf(l1, sizeof(l1), "Eat together now");
        else        snprintf(l1, sizeof(l1), "You:%u  Them:%u", cgBites, dinerBites);
      } else {
        if (sipDue)      snprintf(l1, sizeof(l1), "Offer a drink");
        else if (prompt) snprintf(l1, sizeof(l1), "Eat together now");
        else             snprintf(l1, sizeof(l1), "Sips:%u  Them:%u", sips, dinerBites);
      }
      unsigned long secs = (now - lastDinerBite) / 1000UL;
      unsigned int mins = secs / 60;
      if (mins > 99) { mins = 99; secs = 59; }
      snprintf(l2, sizeof(l2), "Last bite %2u:%02u", mins, (unsigned int)(secs % 60));
      break;
    }
    case SUMMARY:
      snprintf(l1, sizeof(l1), "Them:%u  You:%u", dinerBites, cgBites);
      snprintf(l2, sizeof(l2), "Sips:%u  %lumin", sips, mealMinutes);
      break;
  }
  printLine(0, l1);
  printLine(1, l2);
}

// ---------------- Setup / loop ----------------
void setup() {
  pinMode(PIN_TILT, INPUT_PULLUP);
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_PLATE_LED, OUTPUT);
  pinMode(PIN_BUZZ, OUTPUT);
  Serial.begin(9600);
  lcd.begin(16, 2);
  drawLcd(millis());
  Serial.println(F("Eat-With-Me ready. Calibrate: spoon+cup DOWN, press button."));
}

void loop() {
  unsigned long now = millis();
  uint8_t btn = readButton(now);

  if (btn == 2) {                       // long press: recalibrate from anywhere
    state = CAL_COVERED;
    glowMode = G_OFF;
    Serial.println(F("Recalibrate"));
  }

  switch (state) {
    case CAL_COVERED:
      if (btn == 1) {
        spoon.covered = averageRead(spoon.pin);
        cup.covered   = averageRead(cup.pin);
        Serial.print(F("Covered  spoon=")); Serial.print(spoon.covered);
        Serial.print(F(" cup=")); Serial.println(cup.covered);
        state = CAL_OPEN;
      }
      break;

    case CAL_OPEN:
      if (btn == 1) {
        spoon.open = averageRead(spoon.pin);
        cup.open   = averageRead(cup.pin);
        Serial.print(F("Open     spoon=")); Serial.print(spoon.open);
        Serial.print(F(" cup=")); Serial.println(cup.open);
        if (spoon.open - spoon.covered < MIN_CAL_GAP || cup.open - cup.covered < MIN_CAL_GAP) {
          Serial.println(F("Gap too small: add light or cover the sensor better"));
          state = CAL_ERROR;
        } else {
          spoon.threshold = (spoon.covered + spoon.open) / 2;
          cup.threshold   = (cup.covered + cup.open) / 2;
          Serial.print(F("Thresholds spoon=")); Serial.print(spoon.threshold);
          Serial.print(F(" cup=")); Serial.println(cup.threshold);
          startMeal(now);
        }
      }
      break;

    case CAL_ERROR:
      if (btn == 1) state = CAL_COVERED;
      break;

    case MEAL: {
      // Caregiver bite: tilt held for BITE_HOLD_MS, outside cooldown.
      bool closed = digitalRead(PIN_TILT) == LOW;
      bool tilted = TILT_CLOSED_MEANS_BITE ? closed : !closed;
      if (tilted && !tiltWas) tiltSince = now;
      if (tilted && !tiltCounted && now - tiltSince >= BITE_HOLD_MS &&
          (cgBites == 0 || now - lastCgBite >= BITE_COOLDOWN_MS)) {
        cgBites++;
        lastCgBite = now;
        tiltCounted = true;
        startGlow(now);
        Serial.print(F("Caregiver bite ")); Serial.println(cgBites);
      }
      if (!tilted) tiltCounted = false;
      tiltWas = tilted;

      // Diner picks up spoon.
      if (liftEvent(spoon, now)) {
        dinerBites++;
        lastDinerBite = now;
        lastNudge = now;
        prompt = false;
        stopGlow(now);
        Serial.print(F("Diner bite ")); Serial.println(dinerBites);
      }

      // Diner lifts cup.
      if (liftEvent(cup, now)) {
        sips++;
        lastSip = now;
        sipDue = false;
        Serial.print(F("Sip ")); Serial.println(sips);
      }
      // Drink prompt on the caregiver's LCD after SIP_MS without a sip.
      if (!sipDue && now - lastSip >= SIP_MS) {
        sipDue = true;
        Serial.println(F("Drink reminder"));
      }

      // Nudge after NUDGE_MS with no diner bite, repeating at most every NUDGE_MS.
      if (now - lastDinerBite >= NUDGE_MS && now - lastNudge >= NUDGE_MS) {
        startPulse(now);
        startMelody(now);
        lastNudge = now;
        prompt = true;
        Serial.println(F("Nudge"));
      }

      if (btn == 1) {
        mealMinutes = (now - mealStart) / 60000UL;
        glowMode = G_OFF;
        state = SUMMARY;
        Serial.println(F("Summary"));
      }
      break;
    }

    case SUMMARY:
      if (btn == 1) startMeal(now);
      break;
  }

  updateGlow(now);
  updateMelody(now);
  if (now - lastLcd >= LCD_REFRESH_MS || btn) {
    drawLcd(now);
    lastLcd = now;
  }
}
