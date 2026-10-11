const uint8_t PIN_PIR     = 2;   
const uint8_t PIN_BUZZER  = 4;
const uint8_t PIN_LED_RED = 5;
const uint8_t PIN_LED_GRN = 6;
const uint8_t PIN_ARM     = 8;   // PB0
const uint8_t PIN_TILT    = 9;   // PB1
const uint8_t PIN_LDR     = A0;

const uint8_t MASK_ARM  = (1 << PB0);
const uint8_t MASK_TILT = (1 << PB1);
const uint8_t MASK_BOTH = (MASK_ARM | MASK_TILT);

const uint16_t TIMER1_COMPARE   = 3124;  // 16MHz / (256 * 6250) = 10 Hz
const uint8_t  TICKS_PER_SAMPLE = 5;     // LDR read every 500 ms
const uint8_t  TICKS_PER_LOG    = 20;    // status line every 2 s
const uint16_t DARK_THRESHOLD   = 300;   // tune to your own LDR readings
const uint16_t DEBOUNCE_MS      = 200;
const uint16_t COURTESY_MS      = 3000;
const uint16_t BUZZER_HZ        = 2000;

enum SystemState : uint8_t { STATE_DISARMED = 0, STATE_ARMED = 1, STATE_ALARM = 2 };

// single byte, so the Timer1 ISR can read it atomically without cli()
volatile SystemState systemState = STATE_DISARMED;
volatile bool courtesyActive = false;

// written in an ISR, read in loop() - volatile forces a reload on each access
volatile bool     flagMotion    = false;
volatile bool     flagArmPress  = false;
volatile bool     flagTamper    = false;
volatile bool     flagSample    = false;
volatile bool     flagHeartbeat = false;
volatile uint32_t motionCount   = 0;
volatile uint32_t tamperCount   = 0;

volatile uint8_t  lastPortB  = MASK_BOTH;   // previous PORT B state, for edge detection
volatile uint32_t lastArmMs  = 0;
volatile uint32_t lastTiltMs = 0;

uint16_t lightLevel    = 1023;
bool     isDark        = false;
uint32_t courtesyStart = 0;
bool     buzzerOn      = false;

void setupPins();
void setupExternalInterrupt();
void setupPinChangeInterrupts();
void setupTimer1();
void readInputs(bool &motion, bool &armPress, bool &tamper, bool &sample, bool &beat);
void decideNextState(bool motion, bool armPress, bool tamper);
void driveActuators();
void logHeartbeat();
void logEvent(const char *tag, const char *detail);
const char *stateName(SystemState s);

void setup() {
  Serial.begin(9600);
  setupPins();

  noInterrupts();   // critical section while interrupt registers are configured
  setupExternalInterrupt();
  setupPinChangeInterrupts();
  setupTimer1();
  interrupts();

  Serial.println(F("SIT315 M1QP - Night-Watch"));
  Serial.println(F("INT0 (D2) + PCINT0/PCINT1 (D8/D9) + Timer1 CTC 10 Hz"));
  Serial.println(F("Press D8 to arm or disarm."));
  logEvent("BOOT", "state = DISARMED");
}

void loop() {
  bool motion, armPress, tamper, sample, beat;

  readInputs(motion, armPress, tamper, sample, beat);

  if (sample) {
    lightLevel = analogRead(PIN_LDR);   // ~100 us, too slow to run inside an ISR
    isDark = (lightLevel < DARK_THRESHOLD);
  }

  decideNextState(motion, armPress, tamper);
  driveActuators();

  if (beat) logHeartbeat();
}

// copies every flag in one critical section so the snapshot is consistent
void readInputs(bool &motion, bool &armPress, bool &tamper, bool &sample, bool &beat) {
  noInterrupts();
  motion   = flagMotion;    flagMotion    = false;
  armPress = flagArmPress;  flagArmPress  = false;
  tamper   = flagTamper;    flagTamper    = false;
  sample   = flagSample;    flagSample    = false;
  beat     = flagHeartbeat; flagHeartbeat = false;
  interrupts();
}

void decideNextState(bool motion, bool armPress, bool tamper) {
  SystemState previous = systemState;

  if (armPress) {
    if (systemState == STATE_ALARM) {
      systemState = STATE_DISARMED;
      logEvent("BUTTON", "alarm acknowledged, system DISARMED");
    } else if (systemState == STATE_ARMED) {
      systemState = STATE_DISARMED;
      logEvent("BUTTON", "system DISARMED");
    } else {
      systemState = STATE_ARMED;
      logEvent("BUTTON", "system ARMED");
    }
  }

  // tamper escalates regardless of light level
  if (tamper) {
    if (systemState == STATE_ARMED) {
      systemState = STATE_ALARM;
      logEvent("TAMPER", "tilt switch moved while armed");
    } else if (systemState == STATE_ALARM) {
      logEvent("TAMPER", "tilt moved, alarm already active");
    } else {
      logEvent("TAMPER", "tilt movement ignored, system disarmed");
    }
  }

  // alarm needs BOTH armed and dark - motion alone is not enough
  if (motion) {
    if (systemState == STATE_ARMED && isDark) {
      systemState = STATE_ALARM;
      logEvent("MOTION", "motion in a dark room while armed -> ALARM");
    } else if (systemState == STATE_ARMED) {
      logEvent("MOTION", "motion ignored, room is bright");
    } else if (systemState == STATE_DISARMED && isDark) {
      courtesyActive = true;
      courtesyStart  = millis();
      logEvent("MOTION", "courtesy light on for 3 s");
    } else if (systemState == STATE_ALARM) {
      logEvent("MOTION", "motion seen, alarm already active");
    } else {
      logEvent("MOTION", "motion ignored, disarmed and room is bright");
    }
  }

  // non-blocking timeout, no delay()
  if (courtesyActive && (millis() - courtesyStart >= COURTESY_MS)) {
    courtesyActive = false;
    logEvent("TIMEOUT", "courtesy light off");
  }

  if (previous != systemState) {
    Serial.print(F("  >> STATE "));
    Serial.print(stateName(previous));
    Serial.print(F(" -> "));
    Serial.println(stateName(systemState));
  }
}

void driveActuators() {
  if (systemState == STATE_ALARM && !buzzerOn) {
    tone(PIN_BUZZER, BUZZER_HZ);   // non-blocking, uses Timer2 not Timer1
    buzzerOn = true;
  } else if (systemState != STATE_ALARM && buzzerOn) {
    noTone(PIN_BUZZER);
    buzzerOn = false;
  }

  if (systemState == STATE_DISARMED) {
    digitalWrite(PIN_LED_GRN, courtesyActive ? HIGH : LOW);
  }
}

// ISR 1 - external interrupt, sets a flag only
void onMotionISR() {
  flagMotion = true;
  motionCount++;
}

// ISR 2 - one vector shared by D8 and D9, so it must identify which pin moved
ISR(PCINT0_vect) {
  uint8_t now     = PINB & MASK_BOTH;   // read the whole port in one instruction
  uint8_t changed = now ^ lastPortB;    // XOR marks every bit that differs
  lastPortB = now;

  uint32_t t = millis();

  if (changed & MASK_ARM) {
    if (t - lastArmMs > DEBOUNCE_MS) {     // reject contact bounce
      lastArmMs = t;
      if (!(now & MASK_ARM)) flagArmPress = true;   // INPUT_PULLUP: press = falling edge
    }
  }

  if (changed & MASK_TILT) {
    if (t - lastTiltMs > DEBOUNCE_MS) {
      lastTiltMs = t;
      flagTamper = true;   // either edge counts as tampering
      tamperCount++;
    }
  }
}

// ISR 3 - Timer1 compare match every 100 ms, independent of any sensor
ISR(TIMER1_COMPA_vect) {
  static uint8_t phase     = 0;
  static uint8_t sampleDiv = 0;
  static uint8_t logDiv    = 0;

  phase = (phase + 1) % 10;

  // only cheap register writes belong here
  switch (systemState) {
    case STATE_ALARM:
      digitalWrite(PIN_LED_RED, (phase % 2) ? HIGH : LOW);
      digitalWrite(PIN_LED_GRN, LOW);
      break;
    case STATE_ARMED:
      digitalWrite(PIN_LED_RED, LOW);
      digitalWrite(PIN_LED_GRN, (phase == 0) ? HIGH : LOW);
      break;
    default:
      digitalWrite(PIN_LED_RED, LOW);
      if (!courtesyActive) digitalWrite(PIN_LED_GRN, LOW);
      break;
  }

  // slow work is deferred to loop() by raising a flag
  if (++sampleDiv >= TICKS_PER_SAMPLE) { sampleDiv = 0; flagSample    = true; }
  if (++logDiv    >= TICKS_PER_LOG)    { logDiv    = 0; flagHeartbeat = true; }
}

void setupPins() {
  pinMode(PIN_PIR,  INPUT);
  pinMode(PIN_ARM,  INPUT_PULLUP);   // internal pull-up, no external resistor
  pinMode(PIN_TILT, INPUT_PULLUP);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GRN, OUTPUT);
  pinMode(PIN_BUZZER,  OUTPUT);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_GRN, LOW);
}

void setupExternalInterrupt() {
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), onMotionISR, RISING);
}

void setupPinChangeInterrupts() {
  lastPortB = PINB & MASK_BOTH;   // snapshot the resting state before enabling
  PCICR  |= (1 << PCIE0);         // enable pin change interrupts on PORT B
  PCMSK0 |= (1 << PCINT0);        // unmask D8
  PCMSK0 |= (1 << PCINT1);        // unmask D9
}

void setupTimer1() {
  TCCR1A = 0;
  TCCR1B = 0;
  TCNT1  = 0;
  OCR1A  = TIMER1_COMPARE;
  TCCR1B |= (1 << WGM12);    // CTC mode, counter clears on compare match
  TCCR1B |= (1 << CS12);     // prescaler 256
  TIMSK1 |= (1 << OCIE1A);   // enable compare match A interrupt
}

void logEvent(const char *tag, const char *detail) {
  Serial.print(F("["));
  Serial.print(millis());
  Serial.print(F(" ms] "));
  Serial.print(tag);
  Serial.print(F(": "));
  Serial.println(detail);
}

void logHeartbeat() {
  Serial.print(F("[")); Serial.print(millis()); Serial.print(F(" ms] STATUS"));
  Serial.print(F("  state="));   Serial.print(stateName(systemState));
  Serial.print(F("  light="));   Serial.print(lightLevel);
  Serial.print(isDark ? F(" (dark)") : F(" (bright)"));
  Serial.print(F("  tilt="));    Serial.print(digitalRead(PIN_TILT) ? F("up") : F("tipped"));
  Serial.print(F("  motions=")); Serial.print(motionCount);
  Serial.print(F("  tampers=")); Serial.println(tamperCount);
}

const char *stateName(SystemState s) {
  switch (s) {
    case STATE_ARMED: return "ARMED";
    case STATE_ALARM: return "ALARM";
    default:          return "DISARMED";
  }
}