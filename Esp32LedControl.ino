#include <WebServer.h>
#include <FastLED.h>
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <ESPmDNS.h>
#include <EEPROM.h>
#include <WiFi.h>

// Konfiguracja Wi-Fi (Brak zakodowanych na stałe danych dostępowych)
const char* hostname = "ledcontrol"; 

// LED Configuration
#define LED_PIN 2
#define MAX_LEDS 298
CRGB leds[MAX_LEDS];
int numLeds = 30;         // Domyślna liczba diod LED
int brightness = 150;     // Domyślna jasność
bool ledPower = true;     // Czy LED są włączone (ON/OFF)
int currentEffect = 0;     // 0 - stały kolor, 1 - tęcza, 2 - fade, 3 - przemieszczające się LED

// Zmienne dla efektów
unsigned long lastUpdate = 0;
unsigned long lastRelease = 0;
const int rainbowSpeed = 50;
const int fadeSpeed = 50;
const int movingLedSpeed = 30;
const int movingLedInterval = 2000;

// Domyślny kolor (dla efektu 0)
CRGB currentColor = CRGB(255, 0, 0);
uint8_t hue = 32;  // Używane w efektach 1 i 2

// Efekt przemieszczających się LED
#define MAX_MOVING_LEDS 20
struct MovingLED {
  int position;
  CRGB color;
};
MovingLED movingLEDs[MAX_MOVING_LEDS];
int numMovingLEDs = 0;

// HTML dla Panelu Sterowania
const char* html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Sterowanie LED</title>
  <style>
    body { font-family: Arial; text-align: center; background-color: #222; color: white; }
    button { padding: 10px; font-size: 16px; margin: 5px; }
    input[type=range] { width: 80%; }
  </style>
</head>
<body>
  <h1>Sterowanie LED</h1>

  <label for="power">Zasilanie:</label>
  <button onclick="setLedPower(1)">Wlacz</button>
  <button onclick="setLedPower(0)">Wylacz</button>
  <br><br>

  <label for="numLeds">Liczba LEDow:</label>
  <input type="number" id="numLeds" value="30" min="1" max="298">
  <button onclick="setNumLeds()">Zastosuj</button>
  <br><br>

  <label for="brightness">Jasnos:</label>
  <input type="range" id="brightness" min="0" max="255" value="150" oninput="setBrightness(this.value)">
  <br><br>

  <label for="color">Kolor (do efektu 0):</label>
  <input type="color" id="color" value="#ff0000">
  <button onclick="setColor()">Zastosuj kolor</button>
  <br><br>

  <button onclick="setEffect(0)">Staly kolor</button>
  <button onclick="setEffect(1)">Tecza</button>
  <button onclick="setEffect(2)">Fade</button>
  <button onclick="setEffect(3)">Przechodzace diody</button>

  <script>
    function setLedPower(val) {
      fetch(`/led_power?value=${val}`);
    }
    function setNumLeds() {
      const numLeds = document.getElementById('numLeds').value;
      fetch(`/set_num_leds?value=${numLeds}`);
    }
    function setBrightness(value) {
      fetch(`/set_brightness?value=${value}`);
    }
    function setColor() {
      const color = document.getElementById('color').value;
      const r = parseInt(color.substr(1, 2), 16);
      const g = parseInt(color.substr(3, 2), 16);
      const b = parseInt(color.substr(5, 2), 16);
      fetch(`/set_color?r=${r}&g=${g}&b=${b}`);
    }
    function setEffect(effect) {
      fetch(`/set_effect?value=${effect}`);
    }
  </script>
</body>
</html>
)rawliteral";

// nRF24 (pilot zdalnego sterowania)
#define CE_PIN 13
#define CSN_PIN 14
RF24 radio(CE_PIN, CSN_PIN);
const byte address[6] = "00001";

// Poprzednie wartości z pilota (do sprawdzania zmian)
static int oldLedPowerPilota   = -1;
static int oldEffectPilota     = -1;
static int oldClapPilota       = -1;
static int oldFullXPilota      = -1;
static int oldFullZPilota      = -1;
static int oldBrightPilota     = -1;
static int oldLedPercentagePilota = -1;

WebServer server(80);

// Adresy EEPROM dla danych dostępowych Wi-Fi
const int SSID_ADDRESS = 0;
const int PASSWORD_ADDRESS = 32;

void setup() {
  // Inicjalizacja taśmy LED
  FastLED.addLeds<WS2812, LED_PIN, GRB>(leds, MAX_LEDS);
  FastLED.setBrightness(brightness);
  FastLED.clear();
  FastLED.show();

  Serial.begin(9600);
  Serial.println("Checking Wi-Fi setup...");

  // Konfiguracja przycisku resetu Wi-Fi
  pinMode(16, INPUT_PULLUP);  // Użycie wewnętrznego rezystora pull-up

  connectToWiFi();  
  

  if (!radio.begin()) {
    Serial.println("Error initializing nRF24L01+! Check your cabling.");
    while (1) {
      delay(1000);
    }
  }
  radio.setPALevel(RF24_PA_HIGH);
  radio.setDataRate(RF24_250KBPS);
  radio.openReadingPipe(0, address);
  radio.startListening();
  Serial.println("nRF24L01+ ready. Receiving data from remote...");


  server.on("/", HTTP_GET, handleRoot);
  server.on("/led_power", HTTP_GET, handleLedPower);
  server.on("/set_num_leds", HTTP_GET, handleSetNumLeds);
  server.on("/set_brightness", HTTP_GET, handleSetBrightness);
  server.on("/set_color", HTTP_GET, handleSetColor);
  server.on("/set_effect", HTTP_GET, handleSetEffect);
  server.begin();
}

void loop() {
  server.handleClient();  
  checkRadio();    
  runEffects();    

 
  if (digitalRead(16) == LOW) {
    Serial.println("Resetting Wi-Fi credentials...");
    EEPROM.begin(512);
    EEPROM.write(SSID_ADDRESS, '\0');
    EEPROM.write(PASSWORD_ADDRESS, '\0');
    EEPROM.commit();
    delay(1000);  
    ESP.restart();  
  }
}
// -------------------- Odbiór danych radiowych ---------------------
void checkRadio() {
  if (radio.available()) {
    char data[32] = "";
    radio.read(&data, sizeof(data));

    // "ledPower:effect:clap:fullX:fullY:brightness:ledPercentage"
    
    Serial.println("Received message: " + String(data));

    char* token = strtok(data, ":");
    if (!token) {
      Serial.println("Invalid data format.");
      return;
    }
    int newLedPower = atoi(token);

    token = strtok(NULL, ":");
    if (!token) {
      Serial.println("Invalid data format.");
      return;
    }
    int newEffect = atoi(token);

    token = strtok(NULL, ":");
    if (!token) {
      Serial.println("Invalid data format.");
      return;
    }
    int newClap = atoi(token);

    token = strtok(NULL, ":");
    if (!token) {
      Serial.println("Invalid data format.");
      return;
    }
    int newFullX = atoi(token);

    token = strtok(NULL, ":");
    if (!token) {
      Serial.println("Invalid data format.");
      return;
    }
    int newFullZ = atoi(token);

    token = strtok(NULL, ":");
    if (!token) {
      Serial.println("Invalid data format.");
      return;
    }
    int newBright = atoi(token);

  
    int newLedPercentage = 0;
    token = strtok(NULL, ":");
    if (token) {
      newLedPercentage = atoi(token);
    }

    bool changed = false;

    
    if (newLedPower != oldLedPowerPilota) {
      ledPower = (newLedPower == 1);
      oldLedPowerPilota = newLedPower;
      changed = true;
      Serial.printf("Remote -> ledPower=%s\n", ledPower?"ON":"OFF");
    }

    
    if (newEffect != oldEffectPilota) {
      if (newEffect >= 0 && newEffect <= 5) {
        currentEffect = newEffect; 
        oldEffectPilota = newEffect;
        changed = true;
        Serial.printf("Remote -> effect=%d\n", currentEffect);
      } else {
        Serial.println("Invalid effect number received.");
      }
    }

   
    if (newClap == 1 && oldClapPilota == 0) {
      blinkTwice();
      changed = true;
      Serial.println("Remote -> clap => blinkTwice");
    }
    oldClapPilota = newClap;

    
    if (newFullX == 1 && oldFullXPilota == 0) {
      Serial.println("Remote -> full rotation X => effect=4");
      currentEffect = 4; 
      changed = true;
    }
    oldFullXPilota = newFullX;

    
    if (newFullZ == 1 && oldFullZPilota == 0) {
      Serial.println("Remote -> full rotation Y => effect=5");
      currentEffect = 5; 
      changed = true;
    }
    oldFullZPilota = newFullZ;

   
    if (newBright != oldBrightPilota) {
      if (newBright >= 0 && newBright <= 255) {
        brightness = newBright;
        oldBrightPilota = newBright;
        changed = true;
        Serial.printf("Remote -> brightness=%d\n", brightness);
      } else {
        Serial.println("Invalid brightness value received.");
      }
    }

   
    if (newLedPercentage != oldLedPercentagePilota) {
      oldLedPercentagePilota = newLedPercentage;
      int pilotNumLeds = map(newLedPercentage, 0, 100, 0, MAX_LEDS);
      if (pilotNumLeds < 1) pilotNumLeds = 1;
      if (pilotNumLeds > MAX_LEDS) pilotNumLeds = MAX_LEDS;
      numLeds = pilotNumLeds;

      changed = true;
      Serial.printf("Remote -> ledPercentage=%d => numLeds=%d\n", newLedPercentage, numLeds);
    }

    if (changed) {
      Serial.println("Updated parameters from remote (7 fields).");
    }
  }
}

// -------------------- Dwukrotne mignięcie (klaskanie) -------------
void blinkTwice() {
  bool savedLedPower = ledPower;
  int savedBrightness = brightness;
  int savedEffect = currentEffect;

 
  for (int i = 0; i < 2; i++) {
    fill_solid(leds, numLeds, CRGB::White);
    FastLED.setBrightness(255);
    FastLED.show();
    delay(200);

    fill_solid(leds, numLeds, CRGB::Black);
    FastLED.setBrightness(0);
    FastLED.show();
    delay(200);
  }

  
  ledPower = savedLedPower;
  brightness = savedBrightness;
  currentEffect = savedEffect;

 
  FastLED.setBrightness(ledPower ? brightness : 0);
  if (ledPower) {
    drawCurrentEffectOnce();
  } else {
    FastLED.clear();
    FastLED.show();
  }
}

// -------------------- Główna pętla efektów ----------------------
void runEffects() {
  if (!ledPower) {
    FastLED.clear();
    FastLED.show();
    return;
  }

  unsigned long currentMillis = millis();

  switch (currentEffect) {
    case 0:
     drawCurrentEffectOnce();  
      break;

    case 1: 
      if (currentMillis - lastUpdate >= (unsigned long)rainbowSpeed) {
        lastUpdate = currentMillis;       
        fill_solid(leds, MAX_LEDS, CRGB::Black);
        fill_rainbow(leds, numLeds, hue++, 7);
        FastLED.setBrightness(brightness);
        FastLED.show();
      }
      break;

    case 2:  
      if (currentMillis - lastUpdate >= (unsigned long)fadeSpeed) {
        lastUpdate = currentMillis;
        fill_solid(leds, MAX_LEDS, CRGB::Black);
        fill_solid(leds, numLeds, CHSV(hue++, 255, 255));
        FastLED.setBrightness(brightness);
        FastLED.show();
      }
      break;

    case 3:  
      if (currentMillis - lastUpdate >= (unsigned long)movingLedSpeed) {
        lastUpdate = currentMillis;
        
        for (int i = 0; i < numMovingLEDs; i++) {
          if (movingLEDs[i].position >= 0 && movingLEDs[i].position < numLeds) {
            if (movingLEDs[i].position > 0) {
              leds[movingLEDs[i].position - 1] = movingLEDs[i].color;
            }
            movingLEDs[i].position++;
            if (movingLEDs[i].position < numLeds) {
              leds[movingLEDs[i].position] = movingLEDs[i].color;
            }
          } else {
            for (int j = i; j < numMovingLEDs - 1; j++) {
              movingLEDs[j] = movingLEDs[j + 1];
            }
            numMovingLEDs--;
            i--;
          }
        }
       
        if ((currentMillis - lastRelease >= (unsigned long)movingLedInterval) 
             && (numMovingLEDs < MAX_MOVING_LEDS)) {
          lastRelease = currentMillis;
          movingLEDs[numMovingLEDs].position = 0;
          movingLEDs[numMovingLEDs].color = CHSV(hue, 255, 255);
          hue += 32;
          numMovingLEDs++;
        }
        FastLED.setBrightness(brightness);
        FastLED.show();
      }
      break;

    case 4: 
      if (currentMillis - lastUpdate >= (unsigned long)movingLedSpeed) {
        lastUpdate = currentMillis;
       
        fill_solid(leds, MAX_LEDS, CRGB::Black);
        for (int i = 0; i < numMovingLEDs; i++) {
          if (movingLEDs[i].position >= 0 && movingLEDs[i].position < numLeds) {
            if (movingLEDs[i].position > 0) {
              leds[movingLEDs[i].position - 1] = movingLEDs[i].color;
            }
            movingLEDs[i].position++;
            if (movingLEDs[i].position < numLeds) {
              leds[movingLEDs[i].position] = movingLEDs[i].color;
            }
          } else {
            for (int j = i; j < numMovingLEDs - 1; j++) {
              movingLEDs[j] = movingLEDs[j + 1];
            }
            numMovingLEDs--;
            i--;
          }
        }
        if ((currentMillis - lastRelease >= (unsigned long)movingLedInterval) 
             && (numMovingLEDs < MAX_MOVING_LEDS)) {
          lastRelease = currentMillis;
          movingLEDs[numMovingLEDs].position = 0;
          movingLEDs[numMovingLEDs].color = CHSV(hue, 255, 255);
          hue += 32;
          numMovingLEDs++;
        }
        FastLED.setBrightness(brightness);
        FastLED.show();
      }
      break;

    case 5: 
      if (currentMillis - lastUpdate >= 200) {
        lastUpdate = currentMillis;
        fill_solid(leds, MAX_LEDS, CRGB::Black);
        
        for (int i = 0; i < numLeds / 5; i++) {
          int idx = random(numLeds);
          leds[idx] = CRGB::White;
        }
        FastLED.setBrightness(brightness);
        FastLED.show();
      }
      break;
        for (int i = 0; i < numLeds; i++) {
        leds[i] = blend(leds[i], currentColor, 128); 
      }
      FastLED.show();
      break;
  }

  
  if (currentEffect == 0) {
    drawCurrentEffectOnce();
  }
}

// -------------------- Rysowanie efektu 0 (stały kolor) -----------
void drawCurrentEffectOnce() {
  
  fill_solid(leds, MAX_LEDS, CRGB::Black);
  fill_solid(leds, numLeds, currentColor);
  FastLED.setBrightness(brightness);
  FastLED.show();
}

void handleRoot() {
  server.send(200, "text/html", html);
}

void handleLedPower() {
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    ledPower = value == 1;
    drawCurrentEffectOnce();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleSetNumLeds() {
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    if (value > 0 && value <= MAX_LEDS) {
      numLeds = value;
      drawCurrentEffectOnce();
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Invalid LED number");
    }
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleSetBrightness() {
  if (server.hasArg("value")) {
    brightness = server.arg("value").toInt();
    drawCurrentEffectOnce();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleSetColor() {
 if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    int r = server.arg("r").toInt();
    int g = server.arg("g").toInt();
    int b = server.arg("b").toInt();
    currentColor = CRGB(r, g, b);
    drawCurrentEffectOnce(); 
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleSetEffect() {
  if (server.hasArg("value")) {
    int value = server.arg("value").toInt();
    if (value >= 0 && value <= 5) {
      currentEffect = value;
      drawCurrentEffectOnce();
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Invalid effect number");
    }
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void startSetupMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP_LED_SETUP", "password123");  
  
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);
  
  startSetupServer();
}

void startSetupServer() {
  server.on("/", HTTP_GET, handleRootSetup);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

void handleRootSetup() {
  String html = "<html><body><h1>Wi-Fi Setup</h1>"
                 "<form action=\"/save\" method=\"POST\">"
                 "<label for=\"ssid\">SSID:</label><input type=\"text\" name=\"ssid\"><br>"
                 "<label for=\"password\">Password:</label><input type=\"password\" name=\"password\"><br>"
                 "<input type=\"submit\" value=\"Save\">"
                 "</form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  Serial.printf("Saving SSID: %s, Password: %s\n", ssid.c_str(), password.c_str());
  
  EEPROM.begin(512);
  for (int i = 0; i < ssid.length(); ++i) {
    EEPROM.write(SSID_ADDRESS + i, ssid[i]);
  }
  EEPROM.write(SSID_ADDRESS + ssid.length(), '\0');  // Null terminate
  
  for (int i = 0; i < password.length(); ++i) {
    EEPROM.write(PASSWORD_ADDRESS + i, password[i]);
  }
  EEPROM.write(PASSWORD_ADDRESS + password.length(), '\0');

  EEPROM.commit();

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
  delay(1000);
  ESP.restart();  
}

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  
  EEPROM.begin(512);
  char ssid[32], password[32];
  
  
  for (int i = 0; i < 32; ++i) {
    ssid[i] = EEPROM.read(SSID_ADDRESS + i);
    if (ssid[i] == '\0') break;
  }
  for (int i = 0; i < 32; ++i) {
    password[i] = EEPROM.read(PASSWORD_ADDRESS + i);
    if (password[i] == '\0') break;
  }

  if (ssid[0] != '\0' && password[0] != '\0') {
    Serial.printf("Connecting to saved Wi-Fi SSID: %s\n", ssid);
    WiFi.begin(ssid, password);
    int connectionTimeout = 30000; 
    int startTime = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < connectionTimeout) {
      delay(500);
      Serial.print(".");
    }
    if(WiFi.status() == WL_CONNECTED) {
      Serial.println("\nConnected to Wi-Fi");
      server.on("/", HTTP_GET, handleRoot); 
      server.begin();
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());

      if (!MDNS.begin(hostname)) {
        Serial.println("Error setting up MDNS responder!");
      } else {
        Serial.println("mDNS responder set up");
        MDNS.addService("http", "tcp", 80);
        server.begin();
        Serial.println("HTTP server started");
      }
    } else {
      Serial.println("\nFailed to connect to Wi-Fi. Entering setup mode.");
      startSetupMode();
    }
  } else {
    Serial.println("No saved Wi-Fi credentials. Starting setup mode...");
    startSetupMode();
  }
}