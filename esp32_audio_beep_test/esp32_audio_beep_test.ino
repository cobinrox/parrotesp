// ===================================================================
// MAX98357A I2S beep test
// Plays a 440 Hz tone for 1 second on boot, then idles.
// No external libraries needed (uses ESP_I2S, built into ESP32 BSP 3.x).
// ===================================================================

#include <ESP_I2S.h>     // built-in (comes with the ESP32 board package, v3.0+)
#include <math.h>

// ----- I2S pin assignments (must match MAX98357A wiring) -----
const int I2S_BCLK = 27;
const int I2S_WS   = 26;   // LRC on MAX98357A
const int I2S_DOUT = 25;   // DIN on MAX98357A

// ----- Audio parameters -----
const int     SAMPLE_RATE = 16000;
const float   TONE_FREQ   = 440.0;   // A4
const int     TONE_MS     = 1000;    // 1 second
const int16_t AMPLITUDE   = 6000;    // out of 32767 — keep modest for first test

I2SClass i2s;

void playTone(float freq, int duration_ms) {
  const int CHUNK = 256;
  int16_t buf[CHUNK];
  const int total_samples = (SAMPLE_RATE * duration_ms) / 1000;
  int written = 0;
  while (written < total_samples) {
    int this_chunk = (total_samples - written < CHUNK)
                       ? (total_samples - written)
                       : CHUNK;
    for (int i = 0; i < this_chunk; i++) {
      float t = (float)(written + i) / SAMPLE_RATE;
      buf[i] = (int16_t)(sinf(2.0f * (float)PI * freq * t) * AMPLITUDE);
    }
    i2s.write((uint8_t*)buf, this_chunk * sizeof(int16_t));
    written += this_chunk;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== MAX98357A I2S beep test ===");

  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT);
  if (!i2s.begin(I2S_MODE_STD,
                 SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_MONO)) {
    Serial.println("ERROR: i2s.begin() failed - halting");
    while (true) { delay(1000); }
  }
  Serial.println("I2S started");

  Serial.print("Playing ");
  Serial.print(TONE_FREQ);
  Serial.print(" Hz for ");
  Serial.print(TONE_MS);
  Serial.println(" ms...");
  playTone(TONE_FREQ, TONE_MS);
  Serial.println("Beep complete - chip will now stay silent");
}

void loop() {
  // idle
}