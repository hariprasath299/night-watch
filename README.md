# Night-Watch: an interrupt-driven Sense-Think-Act system

SIT315 Concurrent and Distributed Programming, Task M1QP (QP4)
Hariprasath Mahalingam, Deakin University

Night-Watch is a room monitor I built on an Arduino Uno in Tinkercad. A push button arms and disarms it. Once armed, it raises an alarm if it sees motion in a dark room or if someone tips the device over, and it prints a status line to the Serial Monitor every two seconds. Three interrupt sources drive it: an external interrupt for the PIR sensor, pin change interrupts for the button and tilt switch, and a Timer1 interrupt for the periodic work.

Tinkercad circuit: [Night-Watch](https://www.tinkercad.com/things/j8TDZwMCI76-complete-interrupt-driven-system-with-timer-logic/editel?returnTo=https%3A%2F%2Fwww.tinkercad.com%2Fdashboard%2Fdesigns%2Fcircuits&sharecode=gwb_i9ohWolVz67XCwW95YGNcsEPPYg9SdHqWBUKsaM)

## Repository contents

```
TaskM1.cpp                        the full Arduino sketch
README.md                         this file
REFLECTION.md                     reflection report
diagrams/breadboard.png           Tinkercad circuit view
diagrams/schematic.png            Tinkercad schematic view
screenshots/serial_monitor.png    10 Hz run showing timer, PCI and INT0 events
screenshots/timer_test_20hz.png   20 Hz timer test
```

## Circuit

![Breadboard view](diagrams/breadboard.png)

![Schematic view](diagrams/schematic.png)

| Pin | Port bit | Interrupt | Device |
|---|---|---|---|
| D2 | PD2 | INT0, rising edge | PIR motion sensor |
| D8 | PB0 | PCINT0, served by PCINT0_vect | Arm button (INPUT_PULLUP) |
| D9 | PB1 | PCINT1, served by PCINT0_vect | Tilt switch (INPUT_PULLUP) |
| A0 | PC0 | none, read every 500 ms on a Timer1 flag | LDR with a 10 kΩ divider |
| D4 | PD4 | output | Buzzer, 2 kHz through `tone()` |
| D5 | PD5 | output | Red LED |
| D6 | PD6 | output | Green LED |

The button and tilt switch use the internal pull-ups, so pressing the button or tipping the switch pulls the pin low.

## How it works

Each pass of `loop()` runs three stages in order:

1. `readInputs()` copies every ISR flag inside one critical section and clears them.
2. `decideNextState()` applies the rules in the table below.
3. `driveActuators()` switches the buzzer and the courtesy light.

The system is a three-state machine:

| Event | DISARMED | ARMED | ALARM |
|---|---|---|---|
| Button on D8 | goes to ARMED | goes to DISARMED | goes to DISARMED (alarm acknowledged) |
| Motion, room dark | green courtesy light for 3 s | goes to ALARM | logged only |
| Motion, room bright | ignored | ignored | logged only |
| Tilt switch moved | ignored | goes to ALARM | logged only |

Motion on its own never raises the alarm. The system has to be armed and the room has to be dark, meaning an LDR reading below 300. The tilt switch ignores the light level, because moving the device counts as tampering whether the lights are on or off.

In ALARM the red LED blinks at 5 Hz and the buzzer sounds at 2 kHz. In ARMED the green LED flashes once a second.

## Interrupts

### INT0: PIR on D2

`attachInterrupt()` fires on the rising edge. The ISR only sets a flag and increments a motion counter.

### PCINT0_vect: button on D8 and tilt switch on D9

Both pins sit on PORT B, so one vector serves them. `setupPinChangeInterrupts()` enables the port with `PCICR |= (1 << PCIE0)` and unmasks both pins in `PCMSK0`. The vector fires on any edge of either pin, so the ISR reads `PINB` once, XORs it with the previous snapshot and checks which bit changed.

Each pin has its own 200 ms debounce window. The button only counts on the falling edge, which is a press. The tilt switch counts on either edge.

### TIMER1_COMPA_vect: 10 Hz tick

Timer1 runs in CTC mode with a 256 prescaler and `OCR1A = 6249`:

```
16 MHz / (256 x 6250) = 10 Hz, one tick every 100 ms
```

The ISR drives the LED blink patterns directly. It also raises `flagSample` every 5 ticks (LDR read, 500 ms) and `flagHeartbeat` every 20 ticks (status line, 2 s). `analogRead()` and Serial printing are too slow to run inside an ISR, so `loop()` does that work when it sees the flag.

I used Timer1 because Timer0 already runs `millis()` and `tone()` uses Timer2. Taking over Timer1 disables PWM on D9 and D10, which doesn't matter here since D9 is an input.

### Shared variables

Every variable an ISR writes is `volatile`. `systemState` and `courtesyActive` are single bytes, so the Timer1 ISR can read them without disabling interrupts. No ISR calls `delay()`.

## Running it

### Tinkercad

1. Open the Tinkercad link above, or build the circuit from the pin table.
2. Open Code, switch the editor to Text, and replace its contents with `TaskM1.cpp`. Don't add `#include <Arduino.h>`, because it causes conflicting-declaration errors in Tinkercad.
3. Click Start Simulation and open the Serial Monitor.
4. While the simulation runs:
    - click the D8 button to arm or disarm
    - click the PIR and drag the object into its range to trigger motion
    - click the LDR and drag its slider to change the light level
    - click the tilt switch to tip it

### Physical Uno

Put `TaskM1.cpp` in a folder called `TaskM1` and rename it `TaskM1.ino`. Upload it from the Arduino IDE with the board set to Arduino Uno, then open the Serial Monitor at 9600 baud.

## Tests

All results below come from the Tinkercad simulation, with the logs copied from the Serial Monitor.

| Test | What I changed | Result |
|---|---|---|
| 1 | Light level, bright to dark | Response changed: motion ignored at 974, ALARM at 54 |
| 2 | Event traffic during a 10 Hz run | Timer unchanged: status lines stayed 1999 to 2000 ms apart |
| 3 | Timer from 10 Hz to 20 Hz | Status gap halved to about 1000 ms, motion response unchanged |

### Tests 1 and 2: one run at 10 Hz

Screenshot: `screenshots/serial_monitor.png`. The system was already armed with the room bright when this part of the log starts.

```
[7096 ms] MOTION: motion ignored, room is bright
[7999 ms] STATUS  state=ARMED  light=974 (bright)  tilt=up  motions=1  tampers=0
[9999 ms] STATUS  state=ARMED  light=841 (bright)  tilt=up  motions=1  tampers=0
[11999 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=1  tampers=0
[12393 ms] MOTION: motion in a dark room while armed -> ALARM
  >> STATE ARMED -> ALARM
[13998 ms] STATUS  state=ALARM  light=54 (dark)  tilt=up  motions=2  tampers=0
[14076 ms] BUTTON: alarm acknowledged, system DISARMED
  >> STATE ALARM -> DISARMED
[15405 ms] BUTTON: system ARMED
  >> STATE DISARMED -> ARMED
[15997 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=2  tampers=0
[17997 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=2  tampers=0
[18843 ms] TAMPER: tilt switch moved while armed
  >> STATE ARMED -> ALARM
[19997 ms] STATUS  state=ALARM  light=54 (dark)  tilt=tipped  motions=2  tampers=1
```

Test 1. With the system armed at light=974, motion printed "motion ignored, room is bright". After I dragged the LDR down to 54, the same motion raised the alarm at 12393 ms. The motion counter went up both times, so the interrupt fired in both cases and the think stage made the decision.

Test 2. Motion, button and tamper events all happened between status lines, and the status lines still landed at 7999, 9999, 11999, 13998, 15997, 17997 and 19997 ms. Every gap is 1999 or 2000 ms. The 1 ms differences come from comparing two separate clocks: the timestamps are `millis()`, which Timer0 updates in 1.024 ms steps, while the ticks come from Timer1.

The same run also shows the tamper path. At 18843 ms, tipping the switch while armed went straight to ALARM, and the next status line shows `tilt=tipped` and `tampers=1`.

### Test 3: timer at 20 Hz

Screenshot: `screenshots/timer_test_20hz.png`. I changed only `TIMER1_COMPARE` (the value loaded into OCR1A) from 6249 to 3124, which gives 16 MHz / (256 x 3125) = 20 Hz. Then I armed the system, made the room dark and triggered the PIR.

```
[4999 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=0  tampers=0
[5998 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=0  tampers=0
[6998 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=0  tampers=0
[7998 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=0  tampers=0
[8997 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=0  tampers=0
[9997 ms] STATUS  state=ARMED  light=54 (dark)  tilt=up  motions=0  tampers=0
[10650 ms] MOTION: motion in a dark room while armed -> ALARM
  >> STATE ARMED -> ALARM
[10996 ms] STATUS  state=ALARM  light=54 (dark)  tilt=up  motions=1  tampers=0
[11997 ms] STATUS  state=ALARM  light=54 (dark)  tilt=up  motions=1  tampers=0
[12996 ms] STATUS  state=ALARM  light=54 (dark)  tilt=up  motions=1  tampers=0
[13996 ms] STATUS  state=ALARM  light=54 (dark)  tilt=up  motions=1  tampers=0
[14995 ms] STATUS  state=ALARM  light=54 (dark)  tilt=up  motions=1  tampers=0
```

The status lines moved to 999 to 1001 ms apart. The heartbeat is still 20 ticks, but each tick is now 50 ms instead of 100 ms. The motion at 10650 ms still went straight to ALARM, and the next status line at 10996 ms stayed on the one-second grid. Changing the timer changed the periodic output and left the event response alone.

The submitted `TaskM1.cpp` is back at 6249 (10 Hz).

## Configuration

These constants sit at the top of `TaskM1.cpp`:

| Constant | Value | What it controls |
|---|---|---|
| `TIMER1_COMPARE` | 6249 | Timer1 compare value, 10 Hz tick |
| `TICKS_PER_SAMPLE` | 5 | LDR read every 500 ms |
| `TICKS_PER_LOG` | 20 | Status line every 2 s |
| `DARK_THRESHOLD` | 300 | LDR readings below this count as dark |
| `DEBOUNCE_MS` | 200 | Debounce window for D8 and D9 |
| `COURTESY_MS` | 3000 | How long the courtesy light stays on |
| `BUZZER_HZ` | 2000 | Buzzer tone |
