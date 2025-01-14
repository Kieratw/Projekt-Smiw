#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Wire.h>
#include "MPU6050_light.h"
#include <LowPower.h>

// -------------------- Piny nRF24 --------------------
#define CE_PIN   7
#define CSN_PIN  8
RF24 radio(CE_PIN, CSN_PIN);
const byte address[6] = "00001";

// -------------------- Piny przycisków i czujników --------------------
#define LOW_POWER_PIN     3  // Przycisk Low Power (INT1)
#define LED_BUTTON_PIN    4  // Przycisk włącz/wyłącz LED
#define EFFECT_BUTTON_PIN 5  // Przycisk zmiany efektu
#define SOUND_SENSOR_PIN  6  // Czujnik dźwięku (klaskanie)
#define POT_PIN           A0 // Potencjometr
#define POT2_PIN          A7 // Drugi potencjometr

// -------------------- MPU --------------------
MPU6050 mpu(Wire);

// Zmienne do liczenia pełnego obrotu wokół osi X i Z
float accumulatedRotationX = 0;  
float accumulatedRotationZ = 0;  
float lastAngleX = 0;           
float lastAngleZ = 0;           

// -------------------- Low Power --------------------
volatile bool lowPowerMode = false; // Flaga - czy wchodzimy w tryb powerDown

// ISR na pinie 3 (INT1)
void toggleLowPowerISR() {
  lowPowerMode = !lowPowerMode;
}

// -------------------- Zmienne stanu --------------------
// ledPower : effect : clap : fullX : fullZ : brightness : ledPercentage
int ledPower   = 0; // 0/1
int effect     = 0; // 0..3
int clap       = 0; // 0/1
int fullX      = 0; // 0/1, czy nastąpił pełen obrót wokół X
int fullZ      = 0; // 0/1, czy nastąpił pełen obrót wokół Z
int brightness = 0; // 0..255

// NOWE: drugi potencjometr (zakres 0..100)
int ledPercentage    = 0; 
int oldLedPercentage = -1; 

// Pomocnicze do klaskania
bool wasClap = false;
unsigned long clapStartTime = 1;
const unsigned long clapDuration = 300; // 300 ms

// Poprzednie wartości do wykrywania zmian
int oldLedPower   = -1;
int oldEffect     = -1;
int oldClap       = -1;
int oldFullX      = -1;
int oldFullZ      = -1;
int oldBrightness = -1;

// ------------------------------------------------------------
void setup() {
  Serial.begin(9600);

  // Piny
  pinMode(LOW_POWER_PIN, INPUT_PULLUP); 
  pinMode(LED_BUTTON_PIN, INPUT_PULLUP);
  pinMode(EFFECT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(SOUND_SENSOR_PIN, INPUT);
  pinMode(POT_PIN, INPUT);
  pinMode(POT2_PIN, INPUT); 

  // Przerwanie na pinie 3 (INT1) - LowPower
  attachInterrupt(digitalPinToInterrupt(LOW_POWER_PIN), toggleLowPowerISR, FALLING);

  // Inicjalizacja nRF24
  if (!radio.begin()) {
  //  Serial.println("nRF24 init fail!");
    while (1);
  }
  radio.openWritingPipe(address);
  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate(RF24_250KBPS);
  radio.stopListening();
  radio.printDetails();
  Serial.println("Radio OK");

  // Inicjalizacja MPU6050
  Wire.begin();
  byte status = mpu.begin();
  if (status != 0) {
  //  Serial.print("MPU init fail, code=");
  //  Serial.println(status);
    while (1);
  }
 // Serial.println("Kalibracja MPU (2s)...");
  delay(2000);
  mpu.calcOffsets();
//  Serial.println("Kalibracja OK");

  // Ustal początkowe kąty
  lastAngleX = mpu.getAngleX();
  lastAngleZ = mpu.getAngleZ();
}

// ------------------------------------------------------------
void loop() {
  // Jeśli tryb Low Power
  if (lowPowerMode) {
    enterLowPowerMode();
   // Serial.println(">>> powerDown, czekam na przerwanie pin3...");
    LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
  //  Serial.println("Wybudzono z powerDown!");
    exitLowPowerMode();
    return;
  }

  // Normalna praca
  checkButtons();
  checkClap();
  checkPot();
  checkPot2();        
  checkFullRotation();
  checkAndSendIfChanged();

  delay(30);
}

// ------------------------------------------------------------
void enterLowPowerMode() {
//  Serial.println("enterLowPowerMode(): radio powerDown...");
  radio.powerDown();
}

// ------------------------------------------------------------
void exitLowPowerMode() {
 // Serial.println("exitLowPowerMode(): radio powerUp...");
  radio.powerUp();
}

// ------------------------------------------------------------
// Odczyt przycisków LED i EFFECT
void checkButtons() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  if (now - lastCheck < 200) return;
  lastCheck = now;

  if (digitalRead(LED_BUTTON_PIN) == LOW) {
    ledPower = !ledPower;
 //   Serial.print("LED => ");
  //  Serial.println(ledPower);
  }

  if (digitalRead(EFFECT_BUTTON_PIN) == LOW) {
    effect++;
    if (effect > 3) effect = 0;
 //   Serial.print("Effect => ");
 //   Serial.println(effect);
  }
}

// ------------------------------------------------------------
// Klaskanie
void checkClap() {
  if (!wasClap && digitalRead(SOUND_SENSOR_PIN) == HIGH) {
    clap = 1;
    wasClap = true;
    clapStartTime = millis();
  //  Serial.println("Klaskanie=1");
  }
  if (wasClap && (millis() - clapStartTime > clapDuration)) {
    clap = 0;
    wasClap = false;
   // Serial.println("Klaskanie=0");
  }
}

// ------------------------------------------------------------
// Potencjometr (próg 10)
void checkPot() {
  static bool firstTime = true;
  int val = analogRead(POT2_PIN);
  int newBright = map(val, 0, 1023, 0, 255);
  if (firstTime || abs(newBright - brightness) >= 10) {
    brightness = newBright;
    firstTime = false;
  }
}

// ------------------------------------------------------------
// Procentowa ilosc ledow
void checkPot2() {
  int val2 = analogRead(POT_PIN);
  // mapujemy na 0..100
  int newLedPercentage = map(val2, 0, 1023, 0, 100);

  
  if (abs(newLedPercentage - ledPercentage) >= 3) {
    ledPercentage = newLedPercentage;
  }
}




// Zliczanie pełnego obrotu o 360° wokół X i wokół Z
void checkFullRotation() {
  mpu.update();
  float currentX = mpu.getAngleX();
  float currentZ = mpu.getAngleZ();

  // Różnica X
  float deltaX = currentX - lastAngleX;
  if (deltaX > 180.0)  deltaX -= 360.0;
  if (deltaX < -180.0) deltaX += 360.0;

  accumulatedRotationX += deltaX;
  lastAngleX = currentX;

  if (fabs(accumulatedRotationX) >= 330.0) {
   // Serial.println("Pełen obrót wokół osi X!");
    fullX = 1;
    accumulatedRotationX = 0;
  }

  // Różnica Z
   mpu.update();
  float currentZ2 = mpu.getAngleZ();

  // Różnica Z
  float deltaZ = currentZ2 - lastAngleZ;
  if (deltaZ > 180.0) deltaZ -= 360.0;
  if (deltaZ < -180.0) deltaZ += 360.0;

  accumulatedRotationZ += deltaZ;
  lastAngleZ = currentZ2;

  if (fabs(accumulatedRotationZ) >= 270.0) {
    Serial.println("Pełen obrót wokół osi Z!");
    fullZ = 1;
    accumulatedRotationZ = 0;
  }
}

// ------------------------------------------------------------
// Wysyłanie danych w formacie:
// ledPower:effect:clap:fullX:fullZ:brightness:ledPercentage
void checkAndSendIfChanged() {
  bool isChanged = false;

  // Sprawdzamy stare vs nowe
  if (ledPower       != oldLedPower)       isChanged = true;
  if (effect         != oldEffect)         isChanged = true;
  if (clap           != oldClap)           isChanged = true;
  if (fullX          != oldFullX)          isChanged = true;
  if (fullZ          != oldFullZ)          isChanged = true;
  if (brightness     != oldBrightness)     isChanged = true;
  if (ledPercentage  != oldLedPercentage)  isChanged = true; // NOWE

  if (isChanged) {
    // Budujemy łańcuch
    String message = String(ledPower) + ":" +
                     String(effect)   + ":" +
                     String(clap)     + ":" +
                     String(fullX)    + ":" +
                     String(fullZ)    + ":" +
                     String(brightness) + ":" +
                     String(ledPercentage); 

    sendRadio(message);

    // Zapamiętujemy stan
    oldLedPower       = ledPower;
    oldEffect         = effect;
    oldClap           = clap;
    oldFullX          = fullX;
    oldFullZ          = fullZ;
    oldBrightness     = brightness;
    oldLedPercentage  = ledPercentage; // NOWE

    // Po wysłaniu jednorazowo informację o pełnym obrocie, kasujemy,
    // by wysłać ponownie dopiero przy kolejnym obrocie
    fullX = 0;
    fullZ = 0;
  }
}

// ------------------------------------------------------------
void sendRadio(const String &msg) {
 // Serial.print("Wysylam: ");
//  Serial.println(msg);

  radio.powerUp();
  bool ok = radio.write(msg.c_str(), msg.length() + 1);
  radio.powerDown();
}
