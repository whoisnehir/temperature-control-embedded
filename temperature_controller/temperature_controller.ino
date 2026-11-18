#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#define LCD_ADDR 0x27
const uint8_t LCD_COLS = 16;  // number of characters per LCD line
const int TEMP_SENSOR_PIN = A0;
const int HEATER_PIN = 9;  // drives the TIP31C transistor that switches the bulb
// Buttons (one leg to the pin, the other to GND)
const int BTN_UP = 2;    // increases the value
const int BTN_DOWN = 3;  // decreases the value
const int BTN_OK = 4;    // OK / Next / Start
const int BTN_BACK = 5;  // Back / Stop / Abort
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, 2);
volatile bool tick1s = false;
// temperature control
float setpoint = 25.0;  // T_set, changed from the menu
// times (in seconds)
int tCool = 60;    // cooling time, SETTABLE from the menu
int heatTime = 0;  // how many seconds it took to reach Tset
// overall system state
enum Mode { MODE_MENU,
            MODE_RUN };
enum Phase { PHASE_IDLE,
             PHASE_HEAT,
             PHASE_COOL,
             PHASE_DONE };
Mode curMode = MODE_MENU;
Phase curPhase = PHASE_IDLE;
// time spent in the current phase (seconds)
int phaseSec = 0;
// menu: 0 = T_set, 1 = tCool, 2 = START
int menuItem = 0;
const int MENU_LAST = 2;  // 0..2
// TIMERS
void setupTimer1_1Hz() {
  noInterrupts();
  TCCR1A = 0;
  TCCR1B = 0;
  OCR1A = 15624;  // 1 s with prescaler 1024
  TCCR1B |= (1 << WGM12);
  TCCR1B |= (1 << CS12) | (1 << CS10);
  TIMSK1 |= (1 << OCIE1A);
  interrupts();
}
ISR(TIMER1_COMPA_vect) {
  tick1s = true;
}
// TEMPERATURE READING
float readTemperatureC() {
  (void)analogRead(TEMP_SENSOR_PIN);  // first reading discarded
  long sum = 0;
  const int N = 32;
  for (int i = 0; i < N; i++) {
    sum += analogRead(TEMP_SENSOR_PIN);
    delayMicroseconds(200);
  }
  float raw = sum / (float)N;
  float voltage = raw * (5.0 / 1023.0);
  float tempC = voltage * 100.0;  // LM35: 10 mV/C
  // debug output on Serial
  Serial.print("raw=");
  Serial.print(raw);
  Serial.print(" V=");
  Serial.print(voltage, 3);
  Serial.print(" T=");
  Serial.print(tempC, 2);
  Serial.println(" C");
  return tempC;
}
// BUTTONS
bool buttonPressed(int pin, const char* name) {
  if (digitalRead(pin) == LOW) {  // INPUT_PULLUP => LOW = pressed
    while (digitalRead(pin) == LOW) {
      delay(10);
    }
    delay(20);  // short debounce
    Serial.print("Button: ");
    Serial.println(name);
    return true;
  }
  return false;
}
// update the menu on the LCD
void showMenu() {
  lcd.clear();
  switch (menuItem) {
    case 0:
      lcd.setCursor(0, 0);
      lcd.print("Set T: ");
      lcd.print(setpoint, 1);
      lcd.print((char)223);
      lcd.print("C");
      lcd.setCursor(0, 1);
      lcd.print("UP/DN +/- OK>Nxt");
      break;
    case 1:
      lcd.setCursor(0, 0);
      lcd.print("tCool: ");
      lcd.print(tCool);
      lcd.print("s");
      lcd.setCursor(0, 1);
      lcd.print("UP/DN +/- OK>Nxt");
      break;
    case 2:
      lcd.setCursor(0, 0);
      lcd.print("Start cycle?");
      lcd.setCursor(0, 1);
      lcd.print("OK=Start BACK=<<");
      break;
  }
}
// button logic
void handleButtons() {
  // UP button
  if (buttonPressed(BTN_UP, "UP")) {
    if (curMode == MODE_MENU) {
      switch (menuItem) {
        case 0:  // T_set
          setpoint += 0.5;
          if (setpoint > 80.0) setpoint = 80.0;
          showMenu();
          break;
        case 1:  // tCool
          tCool += 10;
          if (tCool > 999) tCool = 999;
          showMenu();
          break;
        case 2:
          // nothing
          break;
      }
    }
  }
  // DOWN button
  if (buttonPressed(BTN_DOWN, "DOWN")) {
    if (curMode == MODE_MENU) {
      switch (menuItem) {
        case 0:  // T_set
          setpoint -= 0.5;
          if (setpoint < 10.0) setpoint = 10.0;
          showMenu();
          break;
        case 1:  // tCool
          tCool -= 10;
          if (tCool < 10) tCool = 10;
          showMenu();
          break;
        case 2:
          // nothing
          break;
      }
    }
  }
  // OK button
  if (buttonPressed(BTN_OK, "OK")) {
    if (curMode == MODE_MENU) {
      if (menuItem < MENU_LAST) {
        menuItem++;
        showMenu();
      } else {
        // We are on "Start cycle?" -> check Tset against Tcur
        float tC = readTemperatureC();
        if (setpoint <= tC + 0.5) {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Tset < Tcur!");
          lcd.setCursor(0, 1);
          lcd.print("Tcur=");
          lcd.print(tC, 1);
          lcd.print((char)223);
          lcd.print("C");
          delay(2000);
          showMenu();  // do not start the cycle
        } else {
          // START the cycle
          curMode = MODE_RUN;
          curPhase = PHASE_HEAT;
          phaseSec = 0;
          heatTime = 0;
          digitalWrite(HEATER_PIN, HIGH);  // TIP31C conducts -> bulb on
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Cycle started...");
          delay(800);
        }
      }
    }
  }
  // BACK button
  if (buttonPressed(BTN_BACK, "BACK")) {
    if (curMode == MODE_MENU) {
      if (menuItem > 0) menuItem--;
      else menuItem = MENU_LAST;
      showMenu();
    } else if (curMode == MODE_RUN) {
      // abort the cycle and return to the menu
      curMode = MODE_MENU;
      curPhase = PHASE_IDLE;
      phaseSec = 0;
      digitalWrite(HEATER_PIN, LOW);
      showMenu();
    }
  }
}
// Fills the rest of the current LCD line with spaces,
// so no characters from a previous, longer text stay visible
void padLine(size_t printed) {
  while (printed < LCD_COLS) {
    lcd.print(' ');
    printed++;
  }
}
//run mode display
void showRunScreen(float tC) {
  // Line 1: current T + T_set
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(tC, 1);
  lcd.print((char)223);
  lcd.print("C ");
  lcd.print("S:");
  lcd.print(setpoint, 1);
  lcd.print(" ");
  // Line 2: phase + time in phase / summary
  lcd.setCursor(0, 1);
  size_t printed = 0;  // characters written on line 2
  switch (curPhase) {
    case PHASE_HEAT:
      printed += lcd.print("HEAT t=");
      printed += lcd.print(phaseSec);
      printed += lcd.print("s");
      break;
    case PHASE_COOL:
      printed += lcd.print("COOL t=");
      printed += lcd.print(phaseSec);
      printed += lcd.print("s");
      break;
    case PHASE_DONE:
      printed += lcd.print("DONE Th=");
      printed += lcd.print(heatTime);
      printed += lcd.print("s");
      break;
    default:
      printed += lcd.print("IDLE");
      break;
  }
  padLine(printed);
}
//setup
void setup() {
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting...");
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);  // bulb off at startup
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);
  Serial.begin(9600);
  analogReference(DEFAULT);
  setupTimer1_1Hz();
  delay(500);
  showMenu();
}
void loop() {
  // read the buttons all the time
  handleButtons();
  // every 1 s run the main logic
  if (tick1s) {
    tick1s = false;
    float tC = readTemperatureC();
    if (curMode == MODE_RUN) {
      phaseSec++;  // time in the current phase
      switch (curPhase) {
        case PHASE_HEAT:
          // in HEAT: the bulb is ON until we reach Tset
          digitalWrite(HEATER_PIN, HIGH);
          if (tC >= setpoint) {
            // reached the desired temperature
            heatTime = phaseSec;    // store how long it took
            curPhase = PHASE_COOL;  // switch to cooling
            phaseSec = 0;
            digitalWrite(HEATER_PIN, LOW);  // turn the bulb off IMMEDIATELY
          }
          break;
        case PHASE_COOL:
          // in COOL: the bulb is off, we only time the cooling
          digitalWrite(HEATER_PIN, LOW);
          if (phaseSec >= tCool) {
            curPhase = PHASE_DONE;
          }
          break;
        case PHASE_DONE:
          // cycle finished, bulb off; press BACK to return to the menu
          digitalWrite(HEATER_PIN, LOW);
          break;
        default:
          digitalWrite(HEATER_PIN, LOW);
          break;
      }
      // run display
      showRunScreen(tC);
    } else {
      // in menu, the bulb stays off (safety)
      digitalWrite(HEATER_PIN, LOW);
    }
  }
}