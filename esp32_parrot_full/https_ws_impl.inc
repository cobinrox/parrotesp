// ---- Included from esp32_parrot_full.ino after startPlayback() ----
#include <stdlib.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void lastRecCloseAndPatch(void);

// HTTPS (esp_https_server) + WebSocket (/ws) helpers and URI handlers.

static bool loadTlsPemFromFs() {
  if (g_cert_pem && g_key_pem) return true;
  File fc = LittleFS.open("/cert.pem", "r");
  File fk = LittleFS.open("/key.pem", "r");
  if (!fc || !fk) {
    if (fc) fc.close();
    if (fk) fk.close();
    return false;
  }
  size_t nc = fc.size();
  size_t nk = fk.size();
  g_cert_pem = (char*)malloc(nc + 1);
  g_key_pem  = (char*)malloc(nk + 1);
  if (!g_cert_pem || !g_key_pem) {
    free(g_cert_pem); g_cert_pem = nullptr;
    free(g_key_pem);  g_key_pem  = nullptr;
    fc.close(); fk.close();
    return false;
  }
  fc.readBytes(g_cert_pem, nc); g_cert_pem[nc] = '\0';
  fk.readBytes(g_key_pem, nk); g_key_pem[nk] = '\0';
  fc.close();
  fk.close();
  return true;
}

static esp_err_t send_json(httpd_req_t *req, int code, const String &body) {
  const char *st = "200 OK";
  if (code == 400) st = "400 Bad Request";
  else if (code == 404) st = "404 Not Found";
  else if (code == 409) st = "409 Conflict";
  else if (code != 200) st = "500 Internal Server Error";
  httpd_resp_set_status(req, st);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static bool get_query_param(httpd_req_t *req, const char *key, char *out, size_t outLen) {
  char q[256];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, outLen) == ESP_OK;
}

static bool guardBeakOrReject(httpd_req_t *req) {
  if (beakLocked()) {
    String body = String("{\"error\":\"beak busy\",\"owner\":\"") + beakAnim.owner + "\"}";
    send_json(req, 409, body);
    return false;
  }
  return true;
}

static esp_err_t uri_root(httpd_req_t *req) {
  if (!LittleFS.exists("/index.html")) {
    const char *msg =
        "ParrotPi (HTTPS)\nNo /index.html on LittleFS.\nUpload data/ then LittleFS upload.\n";
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_set_type(req, "text/html");
  File f = LittleFS.open("/index.html", "r");
  if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open");
  uint8_t buf[512];
  size_t n;
  esp_err_t res = ESP_OK;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    res = httpd_resp_send_chunk(req, (const char *)buf, n);
    if (res != ESP_OK) break;
  }
  f.close();
  if (res == ESP_OK) res = httpd_resp_send_chunk(req, NULL, 0);
  return res;
}

static esp_err_t uri_netinfo(httpd_req_t *req) {
  String body = "{";
  body += "\"mode\":\"AP\",";
  body += "\"scheme\":\"https\",";
  body += "\"ssid\":\""; body += AP_SSID; body += "\",";
  body += "\"ip\":\"";   body += WiFi.softAPIP().toString(); body += "\",";
  body += "\"clients\":"; body += WiFi.softAPgetStationNum(); body += ",";
  body += "\"chip\":\"ESP32-WROOM-32D\",";
  body += "\"mac\":\"";  body += WiFi.softAPmacAddress(); body += "\",";
  body += "\"heap\":";   body += ESP.getFreeHeap(); body += ",";
  body += "\"uptime_ms\":"; body += millis(); body += ",";
  body += "\"ws_path\":\"/ws\"";
  body += "}";
  return send_json(req, 200, body);
}

static esp_err_t uri_on(httpd_req_t *req) {
  setLed(true);
  return send_json(req, 200, String("{\"led\":\"on\"}"));
}
static esp_err_t uri_off(httpd_req_t *req) {
  setLed(false);
  return send_json(req, 200, String("{\"led\":\"off\"}"));
}

static esp_err_t uri_status(httpd_req_t *req) {
  String body = "{";
  body += "\"led\":\"";       body += (ledOn ? "on" : "off");        body += "\",";
  body += "\"beak_deg\":";    body += beakDeg;                       body += ",";
  body += "\"beak_locked\":"; body += (beakLocked() ? "true" : "false"); body += ",";
  body += "\"beak_owner\":\""; body += beakAnim.owner;               body += "\",";
  body += "\"volume\":";      body += String(audioVolume, 2);        body += ",";
  body += "\"pitch\":";       body += String(audioPitch, 2);         body += ",";
  body += "\"streaming\":";   body += (g_wsPcmRatePrimed ? "true" : "false"); body += ",";
  body += "\"talker_ip\":\""; body += g_wsTalkerIp;                 body += "\",";
  body += "\"playing\":\"";   body += currentClip;                   body += "\"";
  body += "}";
  return send_json(req, 200, body);
}

static esp_err_t uri_beak_open(httpd_req_t *req) {
  if (!guardBeakOrReject(req)) return ESP_OK;
  setBeakAngle(BEAK_OPEN_DEG);
  Serial.print("Beak -> "); Serial.print(beakDeg); Serial.println(" deg (manual open)");
  return send_json(req, 200, String("{\"beak\":\"open\"}"));
}
static esp_err_t uri_beak_close(httpd_req_t *req) {
  if (!guardBeakOrReject(req)) return ESP_OK;
  setBeakAngle(BEAK_CLOSED_DEG);
  Serial.print("Beak -> "); Serial.print(beakDeg); Serial.println(" deg (manual close)");
  return send_json(req, 200, String("{\"beak\":\"closed\"}"));
}
static esp_err_t uri_beak_angle(httpd_req_t *req) {
  char degv[16];
  if (!get_query_param(req, "deg", degv, sizeof(degv)))
    return send_json(req, 400, String("{\"error\":\"missing ?deg=N\"}"));
  if (!guardBeakOrReject(req)) return ESP_OK;
  setBeakAngle(atoi(degv));
  Serial.print("Beak -> "); Serial.print(beakDeg); Serial.println(" deg (manual angle)");
  return send_json(req, 200, String("{\"beak_deg\":") + beakDeg + "}");
}

static esp_err_t uri_play(httpd_req_t *req) {
  if (g_wsPcmRatePrimed) {
    return send_json(req, 409, String("{\"error\":\"walkie_stream_active\"}"));
  }
  char clip[64];
  if (!get_query_param(req, "clip", clip, sizeof(clip)))
    return send_json(req, 400, String("{\"error\":\"missing ?clip=NAME\"}"));
  String c = clip;
  if (startPlayback(c))
    return send_json(req, 200, String("{\"playing\":\"") + c + "\"}");
  return send_json(req, 404, String("{\"error\":\"clip not found: ") + c + "\"}");
}

static esp_err_t uri_stop(httpd_req_t *req) {
  stopPlayback();
  return send_json(req, 200, String("{\"playing\":\"\"}"));
}

static esp_err_t uri_clips(httpd_req_t *req) {
  String body = "{\"clips\":[";
  File root = LittleFS.open("/");
  bool first = true;
  if (root) {
    File f = root.openNextFile();
    while (f) {
      String name = f.name();
      if (name.startsWith("/")) name = name.substring(1);
      if (name.endsWith(".wav")) {
        if (!first) body += ",";
        body += "\"";
        body += name.substring(0, name.length() - 4);
        body += "\"";
        first = false;
      }
      f.close();
      f = root.openNextFile();
    }
    root.close();
  }
  body += "]}";
  return send_json(req, 200, body);
}

static esp_err_t uri_volume(httpd_req_t *req) {
  char vs[24];
  if (!get_query_param(req, "v", vs, sizeof(vs)))
    return send_json(req, 400, String("{\"error\":\"missing ?v=N\"}"));
  float v = String(vs).toFloat();
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;
  audioVolume = v;
  if (audioOut) audioOut->SetGain(audioVolume);
  Serial.print("Volume -> "); Serial.println(audioVolume);
  return send_json(req, 200, String("{\"volume\":") + String(audioVolume, 2) + "}");
}

static esp_err_t uri_pitch(httpd_req_t *req) {
  char ps[24];
  if (!get_query_param(req, "p", ps, sizeof(ps)))
    return send_json(req, 400, String("{\"error\":\"missing ?p=N\"}"));
  if (g_wsPcmRatePrimed) {
    return send_json(req, 409, String("{\"error\":\"walkie_stream_active\"}"));
  }
  float p = String(ps).toFloat();
  if (p < 0.5f) p = 0.5f;
  if (p > 2.0f) p = 2.0f;
  audioPitch = p;
  Serial.print("Pitch -> "); Serial.println(audioPitch);
  if (audioOut && audioOut->nativeHertz > 0) {
    audioOut->pitchMultiplier = audioPitch;
    audioOut->SetRate(audioOut->nativeHertz);
  }
  return send_json(req, 200, String("{\"pitch\":") + String(audioPitch, 2) + "}");
}

static esp_err_t uri_restart(httpd_req_t *req) {
  send_json(req, 200, String("{\"restart\":true}"));
  delay(200);
  ESP.restart();
  return ESP_OK;
}

static void ws_pcm_disconnect_cleanup() {
  bool locked = false;
  if (g_walkieAudioMux) {
    if (xSemaphoreTake(g_walkieAudioMux, pdMS_TO_TICKS(2000)) == pdTRUE) {
      locked = true;
    } else {
      Serial.println("[WS] cleanup: walkie mutex timeout");
    }
  }
  g_wsConnectGeneration++;
  wsPcmDiscardQueued();
  lastRecCloseAndPatch();
  const bool hadWalkie = g_wsPcmRatePrimed;
  if (hadWalkie) {
    beakAnim.stop();
    if (audioOut) {
      audioOut->flush();
      audioOut->stop();
    }
  }
  g_wsTalkerIp = "";
  resetWsPcmStreamState();
  if (locked) {
    xSemaphoreGive(g_walkieAudioMux);
  }
}

static esp_err_t parrot_ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in sa = {};
    socklen_t sl = sizeof(sa);
    if (fd >= 0 && getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 &&
        sa.sin_family == AF_INET) {
      uint8_t *ipb = (uint8_t *)&sa.sin_addr.s_addr;
      g_wsTalkerIp =
          String(ipb[0]) + "." + String(ipb[1]) + "." + String(ipb[2]) + "." + String(ipb[3]);
    } else {
      g_wsTalkerIp = "";
    }
    Serial.printf("[WSS] connect fd=%d talker=%s\n", fd, g_wsTalkerIp.c_str());
    if (g_walkieAudioMux &&
        xSemaphoreTake(g_walkieAudioMux, pdMS_TO_TICKS(2000)) == pdTRUE) {
      lastRecCloseAndPatch();
      g_wsConnectGeneration++;
      xSemaphoreGive(g_walkieAudioMux);
    } else {
      g_wsConnectGeneration++;
    }
    return ESP_OK;
  }

  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
  if (ret != ESP_OK) {
    ws_pcm_disconnect_cleanup();
    return ret;
  }

  if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
    Serial.println("[WSS] disconnect (close)");
    ws_pcm_disconnect_cleanup();
    return ESP_OK;
  }

  if (pkt.len > 32 * 1024) {
    return ESP_ERR_INVALID_SIZE;
  }

  uint8_t *buf = (uint8_t *)calloc(1, pkt.len + 1);
  if (!buf && pkt.len > 0) return ESP_ERR_NO_MEM;
  pkt.payload = buf;
  ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
  if (ret != ESP_OK) {
    free(buf);
    ws_pcm_disconnect_cleanup();
    return ret;
  }

  if (pkt.type == HTTPD_WS_TYPE_TEXT) {
    httpd_ws_frame_t out = {};
    out.type = HTTPD_WS_TYPE_TEXT;
    out.payload = pkt.payload;
    out.len = pkt.len;
    ret = httpd_ws_send_frame(req, &out);
    free(buf);
    return ret;
  }

  if (pkt.type == HTTPD_WS_TYPE_BINARY) {
    static const size_t kWsPcmMinBytes = 64;
    if (pkt.len >= kWsPcmMinBytes && (pkt.len & 1u) == 0) {
      WsPcmQueueMsg msg = {buf, pkt.len, g_wsConnectGeneration};
      if (g_wsPcmQ && xQueueSend(g_wsPcmQ, &msg, 0) == pdTRUE) {
        return ESP_OK; // ownership of buf transferred to loop() consumer
      }
      if (g_wsPcmQ) {
        Serial.println("[WS] PCM queue full, draining synchronously");
      }
      feedWsPcmToI2s(pkt.payload, pkt.len, g_wsConnectGeneration);
      free(buf);
      return ESP_OK;
    }
    httpd_ws_frame_t out = {};
    out.type = HTTPD_WS_TYPE_BINARY;
    out.payload = pkt.payload;
    out.len = pkt.len;
    ret = httpd_ws_send_frame(req, &out);
    free(buf);
    return ret;
  }

  free(buf);
  return ESP_OK;
}

static esp_err_t register_parrot_uris(httpd_handle_t hd) {
#if !defined(CONFIG_HTTPD_WS_SUPPORT)
#error "CONFIG_HTTPD_WS_SUPPORT is required for /ws (ESP32 Arduino: sdkconfig must enable httpd WebSocket server)."
#endif
#define UGET(p, fn) {p, HTTP_GET, fn, NULL, false, false, NULL}

  const httpd_uri_t u_root = UGET("/", uri_root);
  const httpd_uri_t u_on = UGET("/on", uri_on);
  const httpd_uri_t u_off = UGET("/off", uri_off);
  const httpd_uri_t u_status = UGET("/status", uri_status);
  const httpd_uri_t u_net = UGET("/netinfo", uri_netinfo);
  const httpd_uri_t u_bo = UGET("/beak/open", uri_beak_open);
  const httpd_uri_t u_bc = UGET("/beak/close", uri_beak_close);
  const httpd_uri_t u_ba = UGET("/beak/angle", uri_beak_angle);
  const httpd_uri_t u_play = UGET("/play", uri_play);
  const httpd_uri_t u_stop = UGET("/stop", uri_stop);
  const httpd_uri_t u_clips = UGET("/clips", uri_clips);
  const httpd_uri_t u_vol = UGET("/volume", uri_volume);
  const httpd_uri_t u_pitch = UGET("/pitch", uri_pitch);
  const httpd_uri_t u_rst = UGET("/restart", uri_restart);
#undef UGET

  const httpd_uri_t u_ws = {
      .uri = "/ws",
      .method = HTTP_GET,
      .handler = parrot_ws_handler,
      .user_ctx = NULL,
      .is_websocket = true,
      .handle_ws_control_frames = true,
      .supported_subprotocol = NULL,
  };

  const httpd_uri_t *const all[] = {&u_root, &u_on, &u_off, &u_status, &u_net, &u_bo, &u_bc, &u_ba,
                                    &u_play, &u_stop, &u_clips, &u_vol, &u_pitch, &u_rst, &u_ws};
  esp_err_t last = ESP_OK;
  for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    esp_err_t r = httpd_register_uri_handler(hd, all[i]);
    if (r != ESP_OK) {
      Serial.printf("HTTPS: register %s failed: %s\n", all[i]->uri, esp_err_to_name(r));
      last = r;
    }
  }
  return last;
}

static esp_err_t startParrotHttps() {
  if (!loadTlsPemFromFs()) {
    Serial.println("HTTPS: missing /cert.pem or /key.pem on LittleFS");
    return ESP_FAIL;
  }
  httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
  conf.servercert = (const uint8_t *)g_cert_pem;
  conf.servercert_len = strlen(g_cert_pem) + 1;
  conf.prvtkey_pem = (const uint8_t *)g_key_pem;
  conf.prvtkey_len = strlen(g_key_pem) + 1;
  conf.httpd.max_uri_handlers = 24;
  conf.httpd.max_open_sockets = 7;
  conf.httpd.server_port = 0;

  esp_err_t err = httpd_ssl_start(&g_https, &conf);
  if (err != ESP_OK) {
    Serial.printf("HTTPS: httpd_ssl_start failed: %s\n", esp_err_to_name(err));
    return err;
  }
  err = register_parrot_uris(g_https);
  if (err != ESP_OK) Serial.printf("HTTPS: register uri err 0x%x\n", (unsigned)err);
  return err;
}
