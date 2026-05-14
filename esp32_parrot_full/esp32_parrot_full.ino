// ===================================================================
// ParrotPi (ESP32 edition)
// WiFi AP + REST server with:
//   GET /on, /off                       - LED on GPIO 4
//   GET /beak/open, /beak/close         - servo on GPIO 18 (rejected while busy)
//   GET /beak/angle?deg=N               - servo to N degrees (rejected while busy)
//   GET /play?clip=NAME                 - play /NAME.wav from LittleFS
//                                         beak chatters while playing, closes at end
//   GET /stop                           - stop current playback
//   GET /clips                          - list available WAV clips (JSON)
//   GET /volume?v=N                     - audio gain 0.0..1.0
//   GET /pitch?p=N                      - pitch 0.5..2.0 (cheap: changes
//                                         playback rate; affects duration)
//   GET /status                         - overall state (JSON)
//   GET /restart                        - reboot the ESP32
//   GET /netinfo                        - network/device info (JSON)
//
// HTTPS (port 443) + WebSocket on same server (wss://<AP_IP>/ws):
//   - REST + index.html over TLS (self-signed cert.pem + key.pem on LittleFS).
//   - Small TEXT / small BINARY WS frames: echoed (bring-up test).
//   - Larger BINARY frames (walkie PCM): 16-bit little-endian mono @ 16 kHz,
//     pushed to I2S via audioOut (library applies current SetGain volume).
//     Ignored while a WAV clip is playing so the decoder keeps the I2S clock.
//
// Beak animator is a standalone module: any audio source (file playback
// today, live walkie-talkie mic stream tomorrow) just calls
// beakAnim.start() at the beginning and beakAnim.stop() at the end.
// While it is active, manual /beak/* endpoints are rejected with HTTP 409.
// ===================================================================

#include <WiFi.h>                         // built-in (ESP32 board package)
#include <LittleFS.h>                     // built-in (filesystem)
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include <ESP32Servo.h>                   // EXTERNAL: install "ESP32Servo by Kevin Harrington, John K. Bennett"
#include <AudioFileSourceLittleFS.h>      // EXTERNAL: install "ESP8266Audio by Earle F. Philhower, III"
#include <AudioGeneratorWAV.h>             //   (all <AudioXxx.h> headers come from the ESP8266Audio package)
#include <AudioOutputI2S.h>               //

const char* VERSION = "0.3";
// 0.3 — HTTPS :443 (esp_https_server) + WSS /ws (httpd WebSocket). Plain HTTP/WebSockets removed.


// ----- WiFi Access Point -----
const char* AP_SSID     = "parrotpi-test";
const char* AP_PASSWORD = "parrot1234";

// ----- Hardware pins -----
const int LED_PIN   = 4;
const int SERVO_PIN = 18;
const int I2S_BCLK  = 27;
const int I2S_LRC   = 26;
const int I2S_DOUT  = 25;

// ----- Servo calibration -----
const int BEAK_CLOSED_DEG = 40;
const int BEAK_OPEN_DEG   = 15;

// ----- Beak chatter timing (ms per open/close phase) -----
const unsigned int BEAK_CHATTER_MS = 120;

// ----- Audio defaults -----
float audioVolume = 0.7f;   // 0.0..1.0  (Pi default tuned for indoor use)
float audioPitch  = 1.0f;   // 0.5..2.0  (1.0 = no shift; changes duration too)

// ----- Pitch-aware I2S output -----
// Subclass that intercepts SetRate so pitch is applied automatically
// whenever the WAV decoder reports a new sample rate.
class PitchAudioOutputI2S : public AudioOutputI2S {
public:
  float pitchMultiplier = 1.0f;   // 1.0 = no shift
  int   nativeHertz     = 0;      // last rate requested by the decoder

  bool SetRate(int hz) override {
    nativeHertz = hz;
    int adjusted = (int)(hz * pitchMultiplier);
    return AudioOutputI2S::SetRate(adjusted);
  }
};

// ----- HTTPS server (ESP-IDF httpd + TLS) -----
static httpd_handle_t g_https = nullptr;

// ----- Forward declaration so BeakAnimator can use setBeakAngle() -----
void setBeakAngle(int deg);

// ----- Global state -----
bool   ledOn   = false;
int    beakDeg = BEAK_CLOSED_DEG;
Servo  beakServo;

// ----- Beak animator (modular; reusable for any audio source) -----
// Simple open/close oscillator. Call start() when audio begins,
// stop() when it ends, and update() every loop iteration.
// While active, beakLocked() returns true and manual control is refused.
struct BeakAnimator {
  bool          active     = false;
  bool          openPhase  = false;
  unsigned long lastToggle = 0;
  unsigned int  intervalMs = BEAK_CHATTER_MS;
  const char*   owner      = "";   // who started it (for /status & debug)

  void start(const char* who, unsigned int interval = BEAK_CHATTER_MS) {
    owner      = who;
    intervalMs = interval;
    active     = true;
    openPhase  = false;
    lastToggle = millis();
    setBeakAngle(BEAK_CLOSED_DEG);
    Serial.print("BeakAnim: start ("); Serial.print(who); Serial.println(")");
  }

  void update() {
    if (!active) return;
    unsigned long now = millis();
    if (now - lastToggle >= intervalMs) {
      openPhase = !openPhase;
      setBeakAngle(openPhase ? BEAK_OPEN_DEG : BEAK_CLOSED_DEG);
      lastToggle = now;
    }
  }

  void stop() {
    if (!active) return;
    active    = false;
    openPhase = false;
    setBeakAngle(BEAK_CLOSED_DEG);
    Serial.print("BeakAnim: stop ("); Serial.print(owner); Serial.println(")");
    owner = "";
  }

  bool isActive() const { return active; }
};

BeakAnimator beakAnim;

// Returns true if some subsystem currently owns the beak.
// Right now that's just audio playback; later it'll also include the live
// walkie-talkie mic stream. Any new owner just calls beakAnim.start()/stop().
bool beakLocked() {
  return beakAnim.isActive();
}

// ----- Audio playback objects -----
AudioFileSourceLittleFS *audioFile = nullptr;
AudioGeneratorWAV       *audioWav  = nullptr;
PitchAudioOutputI2S     *audioOut  = nullptr;  // persistent; reused per clip
String                   currentClip = "";

// TLS PEM (heap) — pointers kept for lifetime of https server
static char *g_cert_pem = nullptr;
static char *g_key_pem  = nullptr;

static void resetWsPcmStreamState();
static void feedWsPcmToI2s(const uint8_t* payload, size_t length);

static const int kWsPcmSampleRateHz = 16000;
static bool g_wsPcmRatePrimed = false;

// ----- LED helper -----
void setLed(bool on) {
  ledOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  Serial.print("LED -> "); Serial.println(on ? "ON" : "OFF");
}

// ----- Servo helper -----
void setBeakAngle(int deg) {
  if (deg < 0)   deg = 0;
  if (deg > 180) deg = 180;
  beakDeg = deg;
  beakServo.write(deg);
}

// ----- Audio helpers -----
void stopPlayback() {
  if (audioWav) {
    if (audioWav->isRunning()) audioWav->stop();
    delete audioWav;
    audioWav = nullptr;
  }
  if (audioFile) {
    delete audioFile;
    audioFile = nullptr;
  }
  if (currentClip.length() > 0) {
    // Hand the beak back regardless of how playback ended (natural end, /stop,
    // /restart, or another /play interrupting). Idempotent if not active.
    beakAnim.stop();
  }
  currentClip = "";
  resetWsPcmStreamState();
  // PCM path leaves I2S running without a WAV generator; stop the driver so DMA does not underrun.
  if (audioOut) audioOut->stop();
}

// ----- WebSocket PCM → I2S (step 3): 16 kHz mono int16 LE, volume via SetGain -----

static void resetWsPcmStreamState() {
  g_wsPcmRatePrimed = false;
}

static void feedWsPcmToI2s(const uint8_t* payload, size_t length) {
  if (!audioOut || length < 2) return;
  if (audioWav && audioWav->isRunning()) {
    static uint32_t s_lastDropLog;
    uint32_t now = millis();
    if (now - s_lastDropLog > 2000) {
      s_lastDropLog = now;
      Serial.println("[WS] PCM dropped while WAV clip is playing (/stop or wait for end)");
    }
    return;
  }

  if (!g_wsPcmRatePrimed) {
    audioOut->stop();                          // drop leftover WAV-era I2S state
    audioOut->pitchMultiplier = 1.0f;          // PCM path has no pitch shift
    audioOut->SetRate(kWsPcmSampleRateHz);
    // I2S is 16-bit fixed in this library — no SetBitsPerSample API.
    audioOut->SetChannels(1);                  // we're handing it 1 ch of int16
    audioOut->SetOutputModeMono(true);         // MAX98357A is mono
    audioOut->SetGain(audioVolume);
    if (!audioOut->begin()) {
      Serial.println("[WS] audioOut->begin() failed");
      return;
    }
    Serial.printf("[WS] PCM primed: 16 kHz mono int16 (first frame %u B)\n",
                  (unsigned)length);
    g_wsPcmRatePrimed = true;
  }

  const int16_t* samples = reinterpret_cast<const int16_t*>(payload);
  const size_t n = length / sizeof(int16_t);
  int16_t frame[2];
  Serial.printf("[WS] PCM chunk %u B (%u samples)\n",
                (unsigned)length, (unsigned)n);
  for (size_t i = 0; i < n; i++) {
    int16_t s = samples[i];
    frame[0] = s;
    frame[1] = s;
    // ESP32 I2S uses non-blocking writes; ConsumeSample returns false when DMA is full.
    // AudioGeneratorWAV retries in loop() — we must spin/yield here for the same reason.
    unsigned spin = 0;
    while (!audioOut->ConsumeSample(frame)) {
      yield();
      if (++spin > 20000) break;   // avoid wedging if I2S never accepts
    }
    if ((i & 0xff) == 0) yield();
  }

  // Without this, I2S TX keeps clocking with an empty DMA ring → repeated clicks until
  // something else (e.g. WAV loop) feeds samples again. flush drains silence, stop powers down.
  if (audioOut) {
    audioOut->flush();
    audioOut->stop();
  }
  resetWsPcmStreamState();
}

bool startPlayback(const String &clipName) {
  stopPlayback();

  String path = "/" + clipName + ".wav";
  if (!LittleFS.exists(path)) {
    Serial.print("Clip not found: "); Serial.println(path);
    return false;
  }

  audioFile = new AudioFileSourceLittleFS(path.c_str());
  audioWav  = new AudioGeneratorWAV();
  audioOut->SetGain(audioVolume);

  // Apply pitch to the output BEFORE begin() so the multiplier is in
  // effect when AudioGeneratorWAV::begin() calls audioOut->SetRate(wavRate).
  // Cheap pitch shift via sample-rate scaling - also changes duration.
  // TODO: real-time pitch-preserving shift for the live mic stream path.
  audioOut->pitchMultiplier = audioPitch;

  if (!audioWav->begin(audioFile, audioOut)) {
    Serial.println("WAV begin failed");
    stopPlayback();
    return false;
  }

  currentClip = clipName;
  beakAnim.start("audio");

  Serial.print("Playing: "); Serial.print(path);
  Serial.print("  (native rate: "); Serial.print(audioOut->nativeHertz);
  Serial.print(" Hz, pitch x"); Serial.print(audioPitch, 2); Serial.println(")");
  return true;
}

#include "https_ws_impl.inc"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("=== ParrotPi (ESP32) v%s starting ===\n", VERSION);
  // Browsers often open extra TCP connections to :443 that never finish TLS (self-signed cert,
  // speculative connects). mbedTLS then logs -0x7780 (MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE).
  // Harmless if the page and wss://…/ws still work. Mute these tags for a cleaner Serial Monitor;
  // comment out to debug TLS handshakes.
  esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
  esp_log_level_set("esp_https_server", ESP_LOG_NONE);
  esp_log_level_set("httpd", ESP_LOG_NONE);
  // LED
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  // Servo
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  beakServo.setPeriodHertz(50);
  beakServo.attach(SERVO_PIN, 500, 2400);
  setBeakAngle(BEAK_CLOSED_DEG);

  // LittleFS
  if (!LittleFS.begin()) {
    Serial.println("ERROR: LittleFS.begin() failed - audio clips unavailable");
  } else {
    Serial.println("LittleFS mounted");
    Serial.println("[TLS] checking /cert.pem and /key.pem on LittleFS...");
    if (LittleFS.exists("/cert.pem") && LittleFS.exists("/key.pem")) {
      File fc = LittleFS.open("/cert.pem", "r");
      File fk = LittleFS.open("/key.pem", "r");
      Serial.printf("TLS files on flash: cert.pem=%u B, key.pem=%u B (next: HTTPS server)\n",
                    (unsigned)(fc ? fc.size() : 0), (unsigned)(fk ? fk.size() : 0));
      if (fc) fc.close();
      if (fk) fk.close();
    } else {
      Serial.println("WARN: /cert.pem or /key.pem missing — add both to data/ and upload LittleFS before HTTPS");
    }
  }

  // I2S audio output (persistent; reused across clips)
  audioOut = new PitchAudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audioOut->SetOutputModeMono(true);   // mix L+R for single mono MAX98357A
  audioOut->SetGain(audioVolume);
  Serial.println("Audio output ready");

  // WiFi AP
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("ERROR: softAP failed - halting");
    while (true) delay(1000);
  }
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  if (startParrotHttps() != ESP_OK) {
    Serial.println("FATAL: HTTPS server failed to start (check cert.pem + key.pem on LittleFS)");
    while (true) delay(3000);
  }
  Serial.println("HTTPS server on port 443 (WSS path /ws)");

  Serial.println("Join WiFi 'parrotpi-test', then open https://192.168.4.1/ (accept cert warning)");

  // ----- Startup self-test -----
  // Play the bundled test.wav and chatter the beak so you can confirm
  // audio + servo + LittleFS all came up. Failures here are non-fatal:
  // if test.wav isn't on flash, we just log and move on.
  Serial.println("Startup self-test: playing /test.wav ...");
  if (!startPlayback("test")) {
    Serial.println("Startup self-test: /test.wav not found - skipping");
  }
}
void loop() {
  // httpd runs on its own tasks; no server.handleClient() needed.

  // Drive the beak animation (no-op if not active).
  beakAnim.update();

  // Pump audio samples to I2S while a clip is playing.
  if (audioWav && audioWav->isRunning()) {
    if (!audioWav->loop()) {
      stopPlayback();             // this also calls beakAnim.stop() and closes the beak
      Serial.println("Playback ended");
    }
  }
}