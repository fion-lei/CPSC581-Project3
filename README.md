# CPSC 581 Project 3

This Arduino sketch helps a person with dementia (the patient) eat alongside a caregiver. When the caregiver takes a bite, a light by the patient's plate glows, so the caregiver's own eating becomes a gentle cue to eat.
 
## Requirements
 
- Arduino Uno R3
- Arduino IDE with **Arduino AVR Boards** installed
- LiquidCrystal library (included with the Arduino IDE)
## Pin map
 
| Pin | Constant | Connects to | Mode |
| --- | --- | --- | --- |
| D2 | `PIN_TILT` | Tilt ball switch, other leg to GND | `INPUT_PULLUP` |
| D3 | `PIN_BTN` | Push button, other leg to GND | `INPUT_PULLUP` |
| D7 | LCD RS | LCD1602 RS | Output |
| D8 | `PIN_BUZZ` | Passive buzzer | `tone()` |
| D10 | `PIN_PLATE_LED` | 220 Ω → LED → GND | PWM (`analogWrite`) |
| D12 | LCD E | LCD1602 E | Output |
| A0 | `PIN_LDR_SPOON` | Photoresistor divider (spoon) | `analogRead` |
| A1 | `PIN_LDR_CUP` | Photoresistor divider (cup) | `analogRead` |
| A2–A5 | LCD D4–D7 | LCD1602 data pins | Output |
 
The LCD runs in 4-bit mode: `LiquidCrystal lcd(7, 12, A2, A3, A4, A5)`.
 
## Upload
 
1. Select **Tools > Board > Arduino AVR Boards > Arduino Uno**.
2. Select the board's port under **Tools > Port**.
3. Open the sketch and click **Upload**.
4. Open **Serial Monitor** at 9600 baud to see the log.
## Behaviour
 
The sketch is a state machine driven by `millis()`. `loop()` never blocks, apart from a 100 ms averaging read while calibrating.
 
| State | LCD | Button |
| --- | --- | --- |
| `CAL_COVERED` | `Place spoon+cup` / `then press btn` | Short press stores the covered readings (20-sample average) |
| `CAL_OPEN` | `Lift spoon+cup` / `then press btn` | Short press stores the open readings; threshold = midpoint |
| `CAL_ERROR` | `Need more light` / `press to retry` | Short press returns to `CAL_COVERED` |
| `MEAL` | See below | Short press goes to `SUMMARY` |
| `SUMMARY` | `Them:x  You:y` / `Sips:x  Nmin` | Short press starts a new meal |
 
A long press (2 s) in any state returns to `CAL_COVERED`. Calibration fails if either sensor's open minus covered reading is under `MIN_CAL_GAP`.
 
### During `MEAL`
 
- **Caregiver bite:** the tilt switch holds its active state for `BITE_HOLD_MS`, at least `BITE_COOLDOWN_MS` after the last bite. This increments `You`, and the LED fades in over `FADE_MS`, holds for `GLOW_HOLD_MS`, then fades out.
- **Spoon lift:** A0 rises above its threshold for `LIFT_STABLE_MS`. This increments `Them`, resets the `Last bite` timer, clears the eat prompt and fades the LED out.
- **Cup lift:** A1 rises above its threshold for `LIFT_STABLE_MS`. This increments `Sips` and clears the drink prompt.
- **Nudge:** no spoon lift for `NUDGE_MS` triggers 3 LED breaths (`PULSE_PERIOD_MS` each), the melody and the `Eat together now` prompt. It repeats at most once every `NUDGE_MS`.
- **Drink prompt:** no cup lift for `SIP_MS` sets `Offer a drink`.
LCD line 2 always shows `Last bite M:SS` (capped at 99:59). Line 1 alternates every `LINE1_SWAP_MS` between `You:x  Them:y` and `Sips:x  Them:y`, and the active prompts take over those slots. When both prompts are active they take turns.
 
### Melody
 
C5, E5, G5, rest, E5, C6, as eighth notes ending on a quarter note, with a 30% gap after each note. It is played note by note in `updateMelody()` without `delay()`, so sensing, glow and LCD keep updating.
 
## Settings
 
| Setting | Default | Purpose |
| --- | --- | --- |
| `DEMO_MODE` | `1` | `1`: `NUDGE_MS` 20 s, `SIP_MS` 30 s. `0`: 4 min, 10 min |
| `BITE_HOLD_MS` | 500 | Tilt hold time for a bite |
| `BITE_COOLDOWN_MS` | 3000 | Minimum gap between caregiver bites |
| `FADE_MS` | 1500 | LED fade in and out |
| `GLOW_HOLD_MS` | 5000 | LED hold at full brightness |
| `LIFT_STABLE_MS` | 300 | Photoresistor debounce |
| `PULSE_PERIOD_MS` | 2000 | Length of one nudge breath |
| `NUDGE_PULSES` | 3 | Breaths per nudge |
| `DEBOUNCE_MS` | 40 | Button debounce |
| `LONG_PRESS_MS` | 2000 | Long-press time |
| `LCD_REFRESH_MS` | 500 | LCD redraw interval |
| `LINE1_SWAP_MS` | 3000 | Line 1 alternation interval |
| `CHIME_ENABLED` | `true` | Melody on or off |
| `TILT_CLOSED_MEANS_BITE` | `true` | Set to `false` if the switch opens on a bite |
| `MIN_CAL_GAP` | 100 | Minimum calibration difference (ADC counts) |
 
## Serial log
 
At 9600 baud the sketch prints:
 
- calibration readings and thresholds
- `MEAL start` and `Summary`
- `Caregiver bite n`, `Diner bite n` and `Sip n`
- `Drink reminder`, `Nudge` and `Recalibrate`