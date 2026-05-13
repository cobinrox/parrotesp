// ===================================================================
// ParrotPi LED + Beak Servo test
// WiFi AP + REST server controlling an LED and a servo from a browser.
// ===================================================================

#include <WiFi.h>            // built-in (comes with the ESP32 board package)
#include <WebServer.h>       // built-in (comes with the ESP32 board package)
#include <ESP32Servo.h>      // EXTERNAL: install "ESP32Servo by Kevin Harrington, John K. Bennett"
                             //           via Tools -> Manage Libraries... (search "ESP32Servo")

// ----- WiFi Access Point settings -----
const char* AP_SSID     = "parrotpi-test";
const char* AP_PASSWORD = "parrot1234";    // must be >= 8 chars for WPA2

// ----- Hardware pins -----
const int LED_PIN   = 4;
const int SERVO_PIN = 18;

// ----- Servo calibration (degrees) -----
// These are starting guesses; tweak once the servo is on the actual beak.
const int BEAK_CLOSED_DEG = 90;
const int BEAK_OPEN_DEG   = 30;

// ----- Web server on TCP port 80 -----
WebServer server(80);

// ----- State -----
bool  ledOn   = false;
int   beakDeg = BEAK_CLOSED_DEG;
Servo beakServo;

// ----- LED helper -----
void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.print("LED -> ");
  Serial.println(on ? "ON" : "OFF");
}

// ----- Servo helper -----
void setBeakAngle(int deg) {
  if (deg < 0)   deg = 0;
  if (deg > 180) deg = 180;
  beakDeg = deg;
  beakServo.write(deg);
  Serial.print("Beak -> ");
  Serial.print(deg);
  Serial.println(" deg");
}

// ----- HTTP handlers -----
void handleRoot() {
  String msg = "ParrotPi test server\n";
  msg += "GET /on                 -> LED on\n";
  msg += "GET /off                -> LED off\n";
  msg += "GET /status             -> overall state (JSON)\n";
  msg += "GET /beak/open          -> open beak\n";
  msg += "GET /beak/close         -> close beak\n";
  msg += "GET /beak/angle?deg=N   -> set beak to N degrees (0-180)\n";
  server.send(200, "text/plain", msg);
}

void handleOn() {
  setLed(true);
  server.send(200, "application/json", "{\"led\":\"on\"}");
}

void handleOff() {
  setLed(false);
  server.send(200, "application/json", "{\"led\":\"off\"}");
}

void handleStatus() {
  String body = "{";
  body += "\"led\":\"";     body += (ledOn ? "on" : "off"); body += "\",";
  body += "\"beak_deg\":";  body += beakDeg;
  body += "}";
  server.send(200, "application/json", body);
}

void handleBeakOpen() {
  setBeakAngle(BEAK_OPEN_DEG);
  server.send(200, "application/json", "{\"beak\":\"open\"}");
}

void handleBeakClose() {
  setBeakAngle(BEAK_CLOSED_DEG);
  server.send(200, "application/json", "{\"beak\":\"closed\"}");
}

void handleBeakAngle() {
  if (!server.hasArg("deg")) {
    server.send(400, "application/json", "{\"error\":\"missing ?deg=N\"}");
    return;
  }
  int deg = server.arg("deg").toInt();
  setBeakAngle(deg);
  String body = String("{\"beak_deg\":") + beakDeg + "}";
  server.send(200, "application/json", body);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== ParrotPi LED + Beak servo test starting ===");

  // LED
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  // Servo
  // ESP32Servo requires explicit PWM timer allocation; there are 4 timers.
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  beakServo.setPeriodHertz(50);                  // 50 Hz, standard hobby servo
  beakServo.attach(SERVO_PIN, 500, 2400);        // min/max pulse width in microseconds
                                                 // (SG90/MG90S typically 500us..2400us)
  setBeakAngle(BEAK_CLOSED_DEG);                 // snap to closed at startup

  // WiFi access point
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
  if (!ok) {
    Serial.println("ERROR: softAP() failed - halting");
    while (true) { delay(1000); }
  }

  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP SSID:     "); Serial.println(AP_SSID);
  Serial.print("AP password: "); Serial.println(AP_PASSWORD);
  Serial.print("AP IP:       "); Serial.println(ip);

  // REST routes
  server.on("/",             HTTP_GET, handleRoot);
  server.on("/on",           HTTP_GET, handleOn);
  server.on("/off",          HTTP_GET, handleOff);
  server.on("/status",       HTTP_GET, handleStatus);
  server.on("/beak/open",    HTTP_GET, handleBeakOpen);
  server.on("/beak/close",   HTTP_GET, handleBeakClose);
  server.on("/beak/angle",   HTTP_GET, handleBeakAngle);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started on port 80");
  Serial.println("Join WiFi 'parrotpi-test', then http://192.168.4.1/");
}

void loop() {
  server.handleClient();
}