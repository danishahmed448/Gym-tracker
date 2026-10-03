# Soil Rover wiring

Arduino Uno / Nano with an nRF24L01 radio, HX710B pressure sensor, DHT22, RS485 NPK soil probe
and an L298N driving four BO motors. Power is a 3S 18650 pack charged over USB-C by a CN3303
boost charger, with a 20k / 10k divider so the Arduino can read the battery percentage.

- `soil-rover-schematic.svg` / `.png`: the full schematic
- `soil_rover_test/soil_rover_test.ino`: test sketch that exercises every connection
  (libraries: **RF24** by TMRh20, **DHT sensor library** by Adafruit)

## Pin map

| Arduino | Connects to | Notes |
|---|---|---|
| D2 | DHT22 DATA (pin 2) | 10k pull-up to +5V |
| D3 | HX710B SCK | |
| D4 | HX710B OUT | |
| D5 (PWM) | L298N ENA | remove the ENA jumper |
| D6 (PWM) | L298N ENB | remove the ENB jumper |
| D7 / D8 | L298N IN1 / IN2 | left motors M1 + M2 |
| A0 / A1 | L298N IN3 / IN4 | right motors M3 + M4 |
| D9 / D10 | nRF24L01 CE / CSN | |
| D11 / D12 / D13 | nRF24L01 MOSI / MISO / SCK | hardware SPI |
| A2 / A3 | MAX485 RO / DI | SoftwareSerial, 9600 baud |
| A4 | MAX485 DE + RE (tied) | HIGH = send |
| A5 | R1 / R2 midpoint | battery voltage / 3 |
| VIN | LM2596 output, set to 7.5 V | |
| 5V | +5V rail (output) | DHT22, HX710B, MAX485, AMS1117 |

## Power

- USB-C 5 V -> CN3303 module (12.6 V, ~0.74 A) -> 5 A fuse -> 3S BMS + 3 x 18650.
- SW1 after the charger tap. Turn it off while charging.
- VBAT (9.0-12.6 V) -> L298N +12V (470 uF), NPK probe V+, LM2596 (7.5 V -> Arduino VIN), battery divider.
- Arduino 5V pin -> sensors and AMS1117-3.3 -> nRF24L01 (100 uF + 100 nF at the module). Never put 5 V on the nRF24L01 VCC.
- One common ground for everything.

## Battery %

`VBAT = analogRead(A5) * 5.0 / 1024 * 3`, then per-cell voltage `VBAT / 3` is looked up in a
Li-ion table (3.30 V = 0 %, 3.84 V = 50 %, 4.20 V = 100 %). See `batteryPercent()` in the sketch.
