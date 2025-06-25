#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <DHT.h>
#include <IRremote.h>  // IR library

/////////////////////////////////
// Pin mappings
/////////////////////////////////
const int relayPins[8]   = {23, 22, 21, 19, 18,  5, 17,  4};
const int switchPins[8]  = {34, 35, 32, 33, 25, 26, 27, 14};
const int led1Pin        = 2;    // Connection indicator LED
const int led2Pin        = 16;   // Timer / boot LED
const int buzzerPin      = 15;   // Buzzer pin
const int DHTPIN         = 13;   // DHT22 data pin
const int DHTTYPE        = DHT22;
const int IR_PIN         = 12;   // IR receiver pin

/////////////////////////////////
// EEPROM layout
/////////////////////////////////
const int NUM_RELAYS       = 8;
const int SSID_MAX_LEN     = 32;
const int PASS_MAX_LEN     = 64;
const int RELAY_EEPROM_ADDR = 0;                               // 0..7: each byte stores one relay state
const int SSID_EEPROM_ADDR  = RELAY_EEPROM_ADDR + NUM_RELAYS;   // 8..39
const int PASS_EEPROM_ADDR  = SSID_EEPROM_ADDR + SSID_MAX_LEN;  // 40..103
const int EEPROM_SIZE      = PASS_EEPROM_ADDR + PASS_MAX_LEN;  // 104; round up to 128

/////////////////////////////////
// Global variables
/////////////////////////////////
bool        relayStates[NUM_RELAYS]       = {false};
int         lastSwitchState[NUM_RELAYS]   = {HIGH};
bool        timerActive[NUM_RELAYS]       = {false};
unsigned long timerEndTime[NUM_RELAYS]    = {0};

bool        buzzerMuted     = false;
String      currentSSID;
String      currentPassword;

WebServer   server(80);
DHT         dht(DHTPIN, DHTTYPE);
IRrecv      irrecv(IR_PIN);
decode_results irResults;

/////////////////////////////////
// Blink / buzzer timing
/////////////////////////////////
unsigned long blinkPreviousMillis = 0;
const unsigned long blinkInterval = 500; // 500 ms on/off
bool blinkState = false;

/////////////////////////////////
// IR Remote NEC codes (example mapping 1–8 keys to relays 0–7)
/////////////////////////////////
// Replace these with the codes from your specific remote if they differ
const unsigned long IR_CODE_RELAY[NUM_RELAYS] = {
  0xFF30CF,  // '1' key → Relay 1
  0xFF18E7,  // '2' key → Relay 2
  0xFF7A85,  // '3' key → Relay 3
  0xFF10EF,  // '4' key → Relay 4
  0xFF38C7,  // '5' key → Relay 5
  0xFF5AA5,  // '6' key → Relay 6
  0xFF42BD,  // '7' key → Relay 7
  0xFF4AB5   // '8' key → Relay 8
};

/////////////////////////////////
// Function Prototypes
/////////////////////////////////
void updateRelayEEPROM(int idx);
String readStringFromEEPROM(int addr, int maxLen);
void writeStringToEEPROM(int addr, int maxLen, const String &str);
bool anyTimerRunning();
String generateHTML();
void handleRoot();
void handleToggle();
void handleSetTimer();
void handleToggleMute();
void handleUpdateAP();

/////////////////////////////////
// Setup
/////////////////////////////////
void setup() {
  Serial.begin(115200);
  delay(100);

  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // Restore relay states from EEPROM
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    byte val = EEPROM.read(RELAY_EEPROM_ADDR + i);
    relayStates[i] = (val == 1);
    digitalWrite(relayPins[i], relayStates[i] ? HIGH : LOW);
    timerActive[i] = false;
  }

  // Restore AP SSID and password from EEPROM (or set defaults)
  currentSSID     = readStringFromEEPROM(SSID_EEPROM_ADDR, SSID_MAX_LEN);
  currentPassword = readStringFromEEPROM(PASS_EEPROM_ADDR, PASS_MAX_LEN);
  if (currentSSID.length() == 0) {
    currentSSID     = "ESP32_RELAY_AP";
    currentPassword = "12345678";
    writeStringToEEPROM(SSID_EEPROM_ADDR, SSID_MAX_LEN, currentSSID);
    writeStringToEEPROM(PASS_EEPROM_ADDR, PASS_MAX_LEN, currentPassword);
  }

  // Initialize DHT22
  dht.begin();

  // Initialize IR receiver
  irrecv.enableIRIn();  // Start the IR receiver

  // Initialize switch pins with internal pull-up
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(switchPins[i], INPUT_PULLUP);
    lastSwitchState[i] = digitalRead(switchPins[i]);
  }

  // Initialize LEDs and buzzer pins
  pinMode(led1Pin, OUTPUT);
  pinMode(led2Pin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(led1Pin, LOW);
  digitalWrite(led2Pin, LOW);
  digitalWrite(buzzerPin, LOW);

  // Boot pattern: blink both LEDs and buzzer 3 times (200ms ON, 200ms OFF)
  for (int i = 0; i < 3; i++) {
    digitalWrite(led1Pin, HIGH);
    digitalWrite(led2Pin, HIGH);
    if (!buzzerMuted) digitalWrite(buzzerPin, HIGH);
    delay(200);
    digitalWrite(led1Pin, LOW);
    digitalWrite(led2Pin, LOW);
    digitalWrite(buzzerPin, LOW);
    delay(200);
  }

  // Start Wi-Fi Access Point with stored credentials
  WiFi.softAP(currentSSID.c_str(), currentPassword.c_str());
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP SSID: ");       Serial.println(currentSSID);
  Serial.print("AP Password: ");   Serial.println(currentPassword);
  Serial.print("AP IP address: "); Serial.println(IP);

  // Define web server routes
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/toggle",      HTTP_GET,  handleToggle);
  server.on("/settimer",    HTTP_GET,  handleSetTimer);
  server.on("/togglemute",  HTTP_GET,  handleToggleMute);
  server.on("/updateap",    HTTP_GET,  handleUpdateAP);

  server.begin();
  Serial.println("HTTP server started");
}

/////////////////////////////////
// Main loop
/////////////////////////////////
void loop() {
  unsigned long now = millis();

  // 1) Check manual switches
  for (int i = 0; i < NUM_RELAYS; i++) {
    int currentState = digitalRead(switchPins[i]);
    // Detect falling edge (active LOW)
    if (currentState == LOW && lastSwitchState[i] == HIGH) {
      delay(50);  // debounce
      if (digitalRead(switchPins[i]) == LOW) {
        timerActive[i] = false;               // cancel any timer
        relayStates[i] = !relayStates[i];     // toggle relay
        digitalWrite(relayPins[i], relayStates[i] ? HIGH : LOW);
        updateRelayEEPROM(i);
      }
    }
    lastSwitchState[i] = currentState;
  }

  // 2) Check IR remote input
  if (irrecv.decode(&irResults)) {
    unsigned long code = irResults.value;
    // Iterate through our mapping array
    for (int i = 0; i < NUM_RELAYS; i++) {
      if (code == IR_CODE_RELAY[i]) {
        // Toggle relay i
        timerActive[i] = false;
        relayStates[i] = !relayStates[i];
        digitalWrite(relayPins[i], relayStates[i] ? HIGH : LOW);
        updateRelayEEPROM(i);
        break; 
      }
    }
    irrecv.resume(); // Prepare to receive the next value
  }

  // 3) Check timers: turn OFF when elapsed
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (timerActive[i] && now >= timerEndTime[i]) {
      relayStates[i] = false;
      digitalWrite(relayPins[i], LOW);
      updateRelayEEPROM(i);
      timerActive[i] = false;
    }
  }

  // 4) LED & Buzzer behavior
  bool timerRunning    = anyTimerRunning();
  bool clientConnected = (WiFi.softAPgetStationNum() > 0);

  if (now - blinkPreviousMillis >= blinkInterval) {
    blinkPreviousMillis = now;
    blinkState = !blinkState;

    if (timerRunning) {
      // Blink both LEDs and buzzer every 500 ms
      digitalWrite(led1Pin, blinkState ? HIGH : LOW);
      digitalWrite(led2Pin, blinkState ? HIGH : LOW);
      if (!buzzerMuted) digitalWrite(buzzerPin, blinkState ? HIGH : LOW);
      else digitalWrite(buzzerPin, LOW);
    } else {
      // No timer running: handle connection LED
      digitalWrite(led2Pin, LOW);    // LED2 off when no timer
      digitalWrite(buzzerPin, LOW);  // Buzzer off when no timer
      if (clientConnected) {
        // Blink LED1 if at least one station is connected
        digitalWrite(led1Pin, blinkState ? HIGH : LOW);
      } else {
        digitalWrite(led1Pin, LOW);  // LED1 off when no client
      }
    }
  }

  // 5) Handle incoming HTTP requests
  server.handleClient();
}

/////////////////////////////////
// Persist a single relay’s state to EEPROM
/////////////////////////////////
void updateRelayEEPROM(int idx) {
  EEPROM.write(RELAY_EEPROM_ADDR + idx, relayStates[idx] ? 1 : 0);
  EEPROM.commit();
}

/////////////////////////////////
// Read a null-terminated string from EEPROM
/////////////////////////////////
String readStringFromEEPROM(int addr, int maxLen) {
  char buf[maxLen + 1];
  for (int i = 0; i < maxLen; i++) {
    buf[i] = EEPROM.read(addr + i);
    if (buf[i] == '\0') break;
  }
  buf[maxLen] = '\0';
  return String(buf);
}

/////////////////////////////////
// Write a string (with null terminator) into EEPROM
/////////////////////////////////
void writeStringToEEPROM(int addr, int maxLen, const String &str) {
  int len = str.length();
  if (len > maxLen - 1) len = maxLen - 1;
  for (int i = 0; i < len; i++) {
    EEPROM.write(addr + i, str[i]);
  }
  EEPROM.write(addr + len, '\0');
  for (int i = len + 1; i < maxLen; i++) {
    EEPROM.write(addr + i, 0);
  }
  EEPROM.commit();
}

/////////////////////////////////
// Check if any relay timer is active
/////////////////////////////////
bool anyTimerRunning() {
  for (int i = 0; i < NUM_RELAYS; i++) {
    if (timerActive[i]) return true;
  }
  return false;
}

/////////////////////////////////
// Generate the HTML dashboard
/////////////////////////////////
String generateHTML() {
  // Read DHT22 values
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  String tempStr = isnan(t) ? "N/A" : String(t, 1) + " &deg;C";
  String humStr  = isnan(h) ? "N/A" : String(h, 1) + " %";

  String html =
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32 Relay & DHT & IR Dashboard</title>"
    "<style>"
      "body { font-family: Arial, sans-serif; text-align: center; }"
      ".btn { width: 120px; height: 50px; margin: 10px; font-size: 16px; }"
      ".on  { background-color: #4CAF50; color: white; }"
      ".off { background-color: #f44336; color: white; }"
      ".timerInput { width: 60px; margin-left: 10px; }"
      ".timerBtn { padding: 5px 10px; margin-left: 5px; font-size: 14px; }"
      ".auxBtn { background-color: #FF9800; color: white; width: 160px; height: 40px; font-size: 14px; margin: 20px; }"
      ".refresh { background-color: #2196F3; color: white; width: 100px; height: 40px; font-size: 14px; margin: 20px; }"
      ".section { border: 1px solid #ccc; padding: 15px; margin: 20px; border-radius: 8px; }"
    "</style>"
    "</head><body>"
    "<h2>ESP32 Relay Dashboard & DHT22 & IR Control</h2>"

    // DHT22 Display
    "<div class=\"section\"><h3>Temperature & Humidity</h3>"
    "<p>Temperature: " + tempStr + "</p>"
    "<p>Humidity: " + humStr + "</p>"
    "</div>"

    // Relay controls + timer inputs
    "<div class=\"section\"><h3>Relay Controls</h3>";
  for (int i = 0; i < NUM_RELAYS; i++) {
    html += "<div>";
    // ON/OFF toggle button
    html += "<button class=\"btn ";
    html += (relayStates[i] ? "on" : "off");
    html += "\" onclick=\"toggleRelay(" + String(i) + ")\">";
    html += "Relay " + String(i + 1);
    html += (relayStates[i] ? " ON" : " OFF") + String("</button>");
    // Timer input and Start button
    html += " ON for (s): <input type=\"number\" id=\"delay" + String(i) + "\" class=\"timerInput\" min=\"1\" />";
    html += "<button class=\"timerBtn\" onclick=\"setTimer(" + String(i) + ")\">Start</button>";
    html += "</div>";
  }
  html += "<br><button class=\"refresh\" onclick=\"location.reload()\">Refresh</button>";
  html += "</div>";

  // Buzzer Mute/Unmute
  html += "<div class=\"section\"><h3>Buzzer Control</h3>";
  if (buzzerMuted) {
    html += "<button class=\"auxBtn\" onclick=\"toggleMute()\">Unmute Buzzer</button>";
  } else {
    html += "<button class=\"auxBtn\" onclick=\"toggleMute()\">Mute Buzzer</button>";
  }
  html += "</div>";

  // AP Settings
  html += "<div class=\"section\"><h3>Change AP Credentials</h3>"
          "SSID: <input type=\"text\" id=\"newSSID\" value=\"" + currentSSID + "\" /><br><br>"
          "Password: <input type=\"password\" id=\"newPass\" value=\"" + currentPassword + "\" /><br><br>"
          "<button class=\"auxBtn\" onclick=\"updateAP()\">Update AP</button>"
          "</div>";

  // IR Instruction (optional)
  html += "<div class=\"section\"><h3>IR Remote Control</h3>"
          "<p>Use your IR remote’s 1–8 buttons to toggle Relay 1–8 accordingly.</p>"
          "</div>";

  // JavaScript functions
  html +=
    "<script>"
      "function toggleRelay(index) {"
        "fetch('/toggle?relay=' + index).then(response => { if (response.ok) location.reload(); });"
      "}"
      "function setTimer(index) {"
        "let delay = document.getElementById('delay' + index).value;"
        "if (delay === '' || isNaN(delay) || delay < 1) { alert('Enter a valid duration in seconds'); return; }"
        "fetch('/settimer?relay=' + index + '&delay=' + delay).then(response => {"
          "if (response.ok) alert('Relay ' + (index + 1) + ' ON for ' + delay + 's'); else alert('Failed to set timer');"
        "});"
      "}"
      "function toggleMute() {"
        "fetch('/togglemute').then(response => { if (response.ok) location.reload(); });"
      "}"
      "function updateAP() {"
        "let ssid = document.getElementById('newSSID').value;"
        "let pass = document.getElementById('newPass').value;"
        "if (ssid.length < 1) { alert('SSID cannot be empty'); return; }"
        "fetch('/updateap?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)).then(response => {"
          "if (response.ok) { alert('AP credentials updated. Reconnecting...'); setTimeout(() => location.reload(), 2000); }"
          "else alert('Failed to update AP');"
        "});"
      "}"
    "</script>"
    "</body></html>";

  return html;
}

/////////////////////////////////
// Web Handlers
/////////////////////////////////

// Serve dashboard
void handleRoot() {
  server.send(200, "text/html", generateHTML());
}

// Toggle relay ON/OFF manually
void handleToggle() {
  if (!server.hasArg("relay")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  int idx = server.arg("relay").toInt();
  if (idx < 0 || idx >= NUM_RELAYS) {
    server.send(400, "text/plain", "Invalid Relay Index");
    return;
  }
  timerActive[idx] = false;  // cancel any timer
  relayStates[idx] = !relayStates[idx];
  digitalWrite(relayPins[idx], relayStates[idx] ? HIGH : LOW);
  updateRelayEEPROM(idx);
  server.send(200, "text/plain", "OK");
}

// Set relay ON for specified duration (seconds), then OFF
void handleSetTimer() {
  if (!server.hasArg("relay") || !server.hasArg("delay")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  int idx = server.arg("relay").toInt();
  int delaySec = server.arg("delay").toInt();
  if (idx < 0 || idx >= NUM_RELAYS || delaySec < 1) {
    server.send(400, "text/plain", "Invalid Parameters");
    return;
  }
  // Turn relay ON immediately
  relayStates[idx] = true;
  digitalWrite(relayPins[idx], HIGH);
  updateRelayEEPROM(idx);
  // Schedule OFF
  timerActive[idx]   = true;
  timerEndTime[idx]  = millis() + (unsigned long)delaySec * 1000UL;
  server.send(200, "text/plain", "OK");
}

// Mute / Unmute buzzer
void handleToggleMute() {
  buzzerMuted = !buzzerMuted;
  server.send(200, "text/plain", "OK");
}

// Update AP SSID and password
void handleUpdateAP() {
  if (!server.hasArg("ssid") || !server.hasArg("pass")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String newSSID = server.arg("ssid");
  String newPass = server.arg("pass");
  if (newSSID.length() < 1 || newSSID.length() > SSID_MAX_LEN - 1
      || newPass.length() > PASS_MAX_LEN - 1) {
    server.send(400, "text/plain", "Invalid SSID/Password Length");
    return;
  }
  // Write new credentials to EEPROM
  writeStringToEEPROM(SSID_EEPROM_ADDR, SSID_MAX_LEN, newSSID);
  writeStringToEEPROM(PASS_EEPROM_ADDR, PASS_MAX_LEN, newPass);
  currentSSID     = newSSID;
  currentPassword = newPass;
  // Restart AP with new credentials
  WiFi.softAP(currentSSID.c_str(), currentPassword.c_str());
  server.send(200, "text/plain", "OK");
}
