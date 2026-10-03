/*
  Soil Rover - wiring test sketch
  Board : Arduino Uno or Nano (ATmega328P)
  Wiring: hardware/soil-rover/soil-rover-schematic.svg

  Libraries (Arduino IDE > Library Manager):
    - "RF24" by TMRh20
    - "DHT sensor library" by Adafruit (it also installs "Adafruit Unified Sensor")

  Serial Monitor at 115200 baud. Every 2 s it prints battery, DHT22, HX710B and
  NPK readings and sends them over the nRF24L01. Type f / b / l / r to run the
  motors for 1.5 s (forward / back / left / right), s to stop.
*/
#include <SPI.h>
#include <RF24.h>
#include <DHT.h>
#include <SoftwareSerial.h>

// ---------------- Pin map (same as the schematic) ----------------
const uint8_t PIN_DHT      = 2;   // DHT22 DATA (10k pull-up to +5V)
const uint8_t PIN_HX_SCK   = 3;   // HX710B SCK
const uint8_t PIN_HX_OUT   = 4;   // HX710B OUT
const uint8_t PIN_ENA      = 5;   // L298N ENA (PWM) - left motors M1 + M2
const uint8_t PIN_ENB      = 6;   // L298N ENB (PWM) - right motors M3 + M4
const uint8_t PIN_IN1      = 7;   // L298N IN1
const uint8_t PIN_IN2      = 8;   // L298N IN2
const uint8_t PIN_NRF_CE   = 9;   // nRF24L01 CE
const uint8_t PIN_NRF_CSN  = 10;  // nRF24L01 CSN (SPI: D11 MOSI, D12 MISO, D13 SCK)
const uint8_t PIN_IN3      = A0;  // L298N IN3
const uint8_t PIN_IN4      = A1;  // L298N IN4
const uint8_t PIN_RS485_RX = A2;  // MAX485 RO
const uint8_t PIN_RS485_TX = A3;  // MAX485 DI
const uint8_t PIN_RS485_DE = A4;  // MAX485 DE + RE (HIGH = transmit, LOW = receive)
const uint8_t PIN_VBAT     = A5;  // battery divider R1 20k / R2 10k

// ---------------- Battery measurement ----------------
const float VREF  = 5.00;      // measure the Arduino 5V pin with a multimeter and put the value here
const float R_TOP = 20000.0;   // R1, VBAT to A5
const float R_BOT = 10000.0;   // R2, A5 to GND

// Resting voltage of one Li-ion cell at 0 %, 10 %, ... 100 %
const float CELL_V[11] = {3.30, 3.69, 3.73, 3.77, 3.80, 3.84, 3.87, 3.95, 4.02, 4.11, 4.20};

// ---------------- Radio packet ----------------
const uint8_t RADIO_ADDR[6] = "ROVR1";

struct Telemetry {        // 18 bytes, fits in one 32-byte nRF24 payload
  int16_t  temp_x10;      // deg C x 10
  uint16_t hum_x10;       // %RH x 10
  int32_t  pressureRaw;   // HX710B counts
  uint16_t n, p, k;       // mg/kg
  uint16_t batt_mV;
  uint8_t  batt_pct;
  uint8_t  flags;         // bit0 DHT ok, bit1 HX710B ok, bit2 NPK ok
};

RF24 radio(PIN_NRF_CE, PIN_NRF_CSN);
DHT dht(PIN_DHT, DHT22);
SoftwareSerial rs485(PIN_RS485_RX, PIN_RS485_TX);

long hxZero = 0;              // HX710B reading at power-up (tare)
bool radioOk = false;
bool motorsOn = false;
uint32_t motorStartMs = 0;

// ---------------- Battery ----------------
float readBatteryVolts() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 16; i++) sum += analogRead(PIN_VBAT);
  return (sum / 16.0) * VREF / 1024.0 * (R_TOP + R_BOT) / R_BOT;
}

uint8_t batteryPercent(float packVolts) {
  float v = packVolts / 3.0;                  // 3S pack -> volts per cell
  if (v <= CELL_V[0])  return 0;
  if (v >= CELL_V[10]) return 100;
  uint8_t i = 1;
  while (v > CELL_V[i]) i++;
  float frac = (v - CELL_V[i - 1]) / (CELL_V[i] - CELL_V[i - 1]);
  return (uint8_t)((i - 1) * 10 + frac * 10 + 0.5);
}

// ---------------- HX710B (24-bit ADC, bit-banged) ----------------
bool readHX710B(long &value) {
  uint32_t start = millis();
  while (digitalRead(PIN_HX_OUT) == HIGH) {   // OUT goes LOW when a sample is ready
    if (millis() - start > 200) return false;
  }
  uint32_t raw = 0;
  noInterrupts();                             // SCK must not stay HIGH for > 60 us
  for (uint8_t i = 0; i < 24; i++) {
    digitalWrite(PIN_HX_SCK, HIGH);
    delayMicroseconds(1);
    raw = (raw << 1) | digitalRead(PIN_HX_OUT);
    digitalWrite(PIN_HX_SCK, LOW);
    delayMicroseconds(1);
  }
  digitalWrite(PIN_HX_SCK, HIGH);             // 25th pulse: next sample = differential input, 10 Hz
  delayMicroseconds(1);
  digitalWrite(PIN_HX_SCK, LOW);
  interrupts();
  if (raw & 0x800000UL) raw |= 0xFF000000UL;  // sign-extend 24-bit two's complement
  value = (long)raw;
  return true;
}

// ---------------- NPK sensor (Modbus RTU over RS485) ----------------
uint16_t modbusCRC(const uint8_t *buf, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// Reads N, P, K (mg/kg) from holding registers 0x001E..0x0020 at slave address 1
bool readNPK(uint16_t &n, uint16_t &p, uint16_t &k) {
  uint8_t req[8] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x03, 0, 0};
  uint16_t crc = modbusCRC(req, 6);
  req[6] = crc & 0xFF;                        // request = 01 03 00 1E 00 03 65 CD
  req[7] = crc >> 8;

  while (rs485.available()) rs485.read();     // drop stale bytes
  digitalWrite(PIN_RS485_DE, HIGH);           // transmit
  rs485.write(req, sizeof(req));              // returns after the last stop bit
  digitalWrite(PIN_RS485_DE, LOW);            // receive

  uint8_t resp[11];                           // 01 03 06 Nh Nl Ph Pl Kh Kl crcL crcH
  uint8_t len = 0;
  uint32_t start = millis();
  while (len < sizeof(resp) && millis() - start < 500) {
    if (rs485.available()) resp[len++] = rs485.read();
  }
  if (len < sizeof(resp) || resp[0] != 0x01 || resp[1] != 0x03 || resp[2] != 6) return false;
  if (modbusCRC(resp, 9) != (resp[9] | (uint16_t)resp[10] << 8)) return false;
  n = (uint16_t)resp[3] << 8 | resp[4];
  p = (uint16_t)resp[5] << 8 | resp[6];
  k = (uint16_t)resp[7] << 8 | resp[8];
  return true;
}

// ---------------- Motors (L298N) ----------------
// left / right: -255 (full reverse) .. 255 (full forward), 0 = coast
void setMotors(int left, int right) {
  digitalWrite(PIN_IN1, left > 0 ? HIGH : LOW);
  digitalWrite(PIN_IN2, left < 0 ? HIGH : LOW);
  analogWrite(PIN_ENA, constrain(abs(left), 0, 255));
  digitalWrite(PIN_IN3, right > 0 ? HIGH : LOW);
  digitalWrite(PIN_IN4, right < 0 ? HIGH : LOW);
  analogWrite(PIN_ENB, constrain(abs(right), 0, 255));
}

void handleSerial() {
  if (!Serial.available()) return;
  const int S = 180;                          // test speed, 0..255
  char c = Serial.read();
  switch (c) {
    case 'f': setMotors( S,  S); break;
    case 'b': setMotors(-S, -S); break;
    case 'l': setMotors(-S,  S); break;
    case 'r': setMotors( S, -S); break;
    case 's': setMotors( 0,  0); break;
    default: return;                          // ignore newlines and other keys
  }
  motorsOn = (c != 's');
  motorStartMs = millis();
  Serial.print(F("Motors: "));
  Serial.println(c);
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_ENA, OUTPUT);
  pinMode(PIN_ENB, OUTPUT);
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  setMotors(0, 0);

  pinMode(PIN_HX_SCK, OUTPUT);
  digitalWrite(PIN_HX_SCK, LOW);
  pinMode(PIN_HX_OUT, INPUT);

  pinMode(PIN_RS485_DE, OUTPUT);
  digitalWrite(PIN_RS485_DE, LOW);
  rs485.begin(9600);                          // NPK sensor default: 9600 baud, 8N1

  dht.begin();

  radioOk = radio.begin() && radio.isChipConnected();
  if (radioOk) {
    radio.setPALevel(RF24_PA_LOW);
    radio.openWritingPipe(RADIO_ADDR);
    radio.stopListening();
  }

  Serial.println(F("\nSoil Rover wiring test"));
  Serial.println(radioOk ? F("nRF24L01 : found") : F("nRF24L01 : NOT FOUND - check 3.3 V, caps, CE/CSN/SPI wires"));
  delay(500);                                 // let the HX710B settle before taking the zero reading
  if (!readHX710B(hxZero)) Serial.println(F("HX710B   : no data - check OUT/SCK wires"));
  Serial.println(F("Type f / b / l / r to test motors, s to stop."));
}

void loop() {
  handleSerial();
  if (motorsOn && millis() - motorStartMs > 1500) {
    setMotors(0, 0);
    motorsOn = false;
  }

  static uint32_t lastReport = 0;
  if (millis() - lastReport < 2000) return;
  lastReport = millis();

  Telemetry t = {};

  float vbat = readBatteryVolts();
  t.batt_mV = (uint16_t)(vbat * 1000);
  t.batt_pct = batteryPercent(vbat);
  Serial.print(F("\nBattery : "));
  Serial.print(vbat, 2);
  Serial.print(F(" V  "));
  Serial.print(t.batt_pct);
  Serial.println(vbat < 5.0 ? F(" %  (main switch off?)") : F(" %"));

  float tc = dht.readTemperature();
  float rh = dht.readHumidity();
  Serial.print(F("DHT22   : "));
  if (isnan(tc) || isnan(rh)) {
    Serial.println(F("read failed - check DATA wire and 10k pull-up"));
  } else {
    t.temp_x10 = (int16_t)(tc * 10);
    t.hum_x10 = (uint16_t)(rh * 10);
    t.flags |= 1;
    Serial.print(tc, 1);
    Serial.print(F(" C  "));
    Serial.print(rh, 1);
    Serial.println(F(" %RH"));
  }

  long hx;
  Serial.print(F("HX710B  : "));
  if (readHX710B(hx)) {
    t.pressureRaw = hx;
    t.flags |= 2;
    Serial.print(hx);
    Serial.print(F(" counts  (change since power-up: "));
    Serial.print(hx - hxZero);
    Serial.println(')');
  } else {
    Serial.println(F("no data"));
  }

  Serial.print(F("NPK     : "));
  if (readNPK(t.n, t.p, t.k)) {
    t.flags |= 4;
    Serial.print(F("N "));
    Serial.print(t.n);
    Serial.print(F("  P "));
    Serial.print(t.p);
    Serial.print(F("  K "));
    Serial.print(t.k);
    Serial.println(F(" mg/kg"));
  } else {
    Serial.println(F("no reply - check A/B wires, sensor power (VBAT) and DE/RE"));
  }

  if (radioOk) {
    bool acked = radio.write(&t, sizeof(t));
    Serial.println(acked ? F("nRF24   : sent, ACK received") : F("nRF24   : sent, no ACK (normal if no receiver is listening)"));
  }
}
