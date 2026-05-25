#include "esp_camera.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ESP32Servo.h>

// ===================== KULLANICI AYARLARI =====================
const char* WIFI_SSID = "WIFI_ADI";
const char* WIFI_PASS = "WIFI_SIFRE";

// Servo / buzzer pinleri
constexpr int PIN_PAN = 14;
constexpr int PIN_TILT = 15;
constexpr int PIN_BUZZER = 13;

// Kamera LEDC kaynaklarıyla çakışmaması için ayrı buzzer kanalı
constexpr int BUZZER_LEDC_CHANNEL = 4;
constexpr int BUZZER_LEDC_TIMER_BITS = 8;
constexpr int BUZZER_BASE_FREQ = 2000;

// Servo sınırları
constexpr int PAN_MIN = 20;
constexpr int PAN_MAX = 160;
constexpr int TILT_MIN = 30;
constexpr int TILT_MAX = 140;

// Non-blocking yumuşak hareket ayarları
constexpr uint32_t SERVO_STEP_INTERVAL_MS = 20;
constexpr int SERVO_STEP_DEG = 1;

struct Note { int freq; uint16_t ms; };
const Note STARTUP_MELODY[] = {{523,120},{659,120},{784,150},{1046,220}};
const Note ALERT_MELODY[] = {{988,100},{1318,100},{1568,120},{0,40},{1568,140}};
const Note WIFI_WAIT_BEEP[] = {{440,35},{0,160}};

AsyncWebServer server(80);
Servo servoPan;
Servo servoTilt;

int panCurrent = 90;
int tiltCurrent = 90;
int panTarget = 90;
int tiltTarget = 90;
uint32_t lastServoStepMs = 0;

const Note* activeMelody = nullptr;
size_t melodyLen = 0;
size_t melodyIdx = 0;
uint32_t melodyNextMs = 0;
bool melodyLoop = false;
bool buzzerAttached = false;

// ========== AI Thinker ESP32-CAM pin map ==========
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

void ensureBuzzerAttached() {
  if (!buzzerAttached) {
    ledcAttach(PIN_BUZZER, BUZZER_BASE_FREQ, BUZZER_LEDC_TIMER_BITS);
    buzzerAttached = true;
  }
}

void buzzerOff() {
  if (buzzerAttached) {
    ledcWriteTone(PIN_BUZZER, 0);
    ledcDetach(PIN_BUZZER);
    buzzerAttached = false;
  }
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
}

void startMelody(const Note* notes, size_t len, bool loopPlayback = false) {
  activeMelody = notes;
  melodyLen = len;
  melodyIdx = 0;
  melodyNextMs = 0;
  melodyLoop = loopPlayback;
}

void stopMelody() {
  activeMelody = nullptr;
  melodyLen = 0;
  melodyIdx = 0;
  melodyLoop = false;
  buzzerOff();
}

void updateMelodyNonBlocking() {
  if (!activeMelody || melodyLen == 0) return;
  const uint32_t now = millis();
  if (now < melodyNextMs) return;

  const Note n = activeMelody[melodyIdx];
  if (n.freq <= 0) {
    buzzerOff();
  } else {
    ensureBuzzerAttached();
    ledcWriteTone(PIN_BUZZER, n.freq);
  }

  melodyNextMs = now + n.ms;
  melodyIdx++;

  if (melodyIdx >= melodyLen) {
    if (melodyLoop) {
      melodyIdx = 0;
    } else {
      stopMelody();
    }
  }
}

void updateServosNonBlocking() {
  const uint32_t now = millis();
  if (now - lastServoStepMs < SERVO_STEP_INTERVAL_MS) return;
  lastServoStepMs = now;

  panTarget = constrain(panTarget, PAN_MIN, PAN_MAX);
  tiltTarget = constrain(tiltTarget, TILT_MIN, TILT_MAX);

  if (panCurrent < panTarget) panCurrent = min(panCurrent + SERVO_STEP_DEG, panTarget);
  else if (panCurrent > panTarget) panCurrent = max(panCurrent - SERVO_STEP_DEG, panTarget);

  if (tiltCurrent < tiltTarget) tiltCurrent = min(tiltCurrent + SERVO_STEP_DEG, tiltTarget);
  else if (tiltCurrent > tiltTarget) tiltCurrent = max(tiltCurrent - SERVO_STEP_DEG, tiltTarget);

  servoPan.write(panCurrent);
  servoTilt.write(tiltCurrent);
}

class AsyncJpegStreamResponse : public AsyncAbstractResponse {
 private:
  camera_fb_t* _fb = nullptr;
  size_t _index = 0;
  size_t _hdrLen = 0;
  uint8_t _hdrBuf[96];

 public:
  AsyncJpegStreamResponse() {
    _code = 200;
    _contentType = "multipart/x-mixed-replace; boundary=frame";
    _sendContentLength = false;
    _chunked = true;
  }

  ~AsyncJpegStreamResponse() {
    if (_fb) esp_camera_fb_return(_fb);
  }

  bool _sourceValid() const override { return true; }

  size_t _fillBuffer(uint8_t* buf, size_t maxLen) override {
    if (!_fb) {
      _fb = esp_camera_fb_get();
      if (!_fb) return RESPONSE_TRY_AGAIN;
      _index = 0;
      String head = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + String(_fb->len) + "\r\n\r\n";
      _hdrLen = head.length();
      if (_hdrLen > sizeof(_hdrBuf)) {
        esp_camera_fb_return(_fb);
        _fb = nullptr;
        return RESPONSE_TRY_AGAIN;
      }
      memcpy(_hdrBuf, head.c_str(), _hdrLen);
    }

    if (_index < _hdrLen) {
      size_t n = min(maxLen, _hdrLen - _index);
      memcpy(buf, _hdrBuf + _index, n);
      _index += n;
      return n;
    }

    size_t fbIdx = _index - _hdrLen;
    if (fbIdx < _fb->len) {
      size_t n = min(maxLen, _fb->len - fbIdx);
      memcpy(buf, _fb->buf + fbIdx, n);
      _index += n;
      return n;
    }

    size_t tailIdx = _index - _hdrLen - _fb->len;
    if (tailIdx < 2) {
      size_t n = min(maxLen, static_cast<size_t>(2 - tailIdx));
      if (n >= 1) buf[0] = '\r';
      if (n >= 2) buf[1] = '\n';
      _index += n;
      return n;
    }

    esp_camera_fb_return(_fb);
    _fb = nullptr;
    _index = 0;
    _hdrLen = 0;
    return RESPONSE_TRY_AGAIN;
  }
};

void registerRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = R"rawliteral(
<!doctype html><html lang='tr'><head><meta charset='utf-8'/>
<meta name='viewport' content='width=device-width,initial-scale=1'/>
<title>Mariss3D Gözetleme Kulesi</title>
<style>
:root{--bg:#020617;--card:#0b1220;--line:#1e293b;--txt:#e2e8f0;--sub:#94a3b8;--a:#38bdf8;--b:#22c55e;--c:#ef4444}
*{box-sizing:border-box} body{font-family:Inter,Arial,sans-serif;background:radial-gradient(1200px 400px at 50% -10%,#0f172a,var(--bg));color:var(--txt);margin:0}
.wrap{max-width:980px;margin:20px auto;padding:14px}.card{background:linear-gradient(180deg,#0b1220,#0a1020);border:1px solid var(--line);border-radius:16px;padding:14px;margin-bottom:14px}
img{width:100%;border-radius:12px;border:1px solid #334155;min-height:180px}.top{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}
.badge{padding:6px 10px;border-radius:999px;border:1px solid var(--line);font-size:.85rem;color:var(--sub)} .ok{color:#86efac}.warn{color:#facc15}
h1{font-size:1.15rem;margin:0}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.btn{border:none;border-radius:11px;padding:10px 14px;color:#fff;cursor:pointer;font-weight:700}
.p{background:#2563eb}.g{background:#16a34a}.r{background:#dc2626}.btn:active{transform:translateY(1px)}
label{display:block;margin:8px 0 4px;color:var(--sub)} input[type=range]{width:100%}.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:820px){.grid{grid-template-columns:1fr}}
</style></head>
<body><div class='wrap'>
<div class='card'><div class='top'><h1>🎥 Mariss3D Gözetleme Kulesi</h1><div id='st' class='badge warn'>Bağlanıyor...</div></div><img src='/stream' alt='Canlı yayın'/></div>
<div class='grid'><div class='card'><h1>🎛️ Pan/Tilt Kontrol</h1>
<label>Pan: <span id='panv'>90</span>°</label><input id='pan' type='range' min='20' max='160' value='90'/>
<label>Tilt: <span id='tiltv'>90</span>°</label><input id='tilt' type='range' min='30' max='140' value='90'/>
<div class='row'><button class='btn p' onclick='savePos()'>Konuma Git</button><button class='btn g' onclick='center()'>Merkezle</button></div></div>
<div class='card'><h1>🔊 Sesli Etkileşim</h1><div class='row'>
<button class='btn r' onclick='buzz()'>Korna Çal</button>
<button class='btn p' onclick='stopBuzz()'>Sesi Durdur</button>
</div><p style='color:var(--sub)'>Sistem non-blocking çalışır: yayın açıkken kontrol devam eder.</p></div></div></div>
<script>
const pan=document.getElementById('pan'),tilt=document.getElementById('tilt'),st=document.getElementById('st');
pan.oninput=()=>panv.innerText=pan.value; tilt.oninput=()=>tiltv.innerText=tilt.value;
async function savePos(){await fetch(`/move?pan=${pan.value}&tilt=${tilt.value}`)}
async function center(){pan.value=90;tilt.value=90;panv.innerText=90;tiltv.innerText=90;await savePos()}
async function buzz(){await fetch('/buzzer')}
async function stopBuzz(){await fetch('/buzzer/stop')}
async function health(){try{const r=await fetch('/health'); const j=await r.json(); st.textContent=j.ok?`Hazır • ${j.ip}`:'Sorun'; st.className='badge '+(j.ok?'ok':'warn')}catch(e){st.textContent='Bağlantı Yok';st.className='badge warn'}}
setInterval(health,2000); health();
</script></body></html>)rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/move", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (request->hasParam("pan")) panTarget = request->getParam("pan")->value().toInt();
    if (request->hasParam("tilt")) tiltTarget = request->getParam("tilt")->value().toInt();
    request->send(200, "text/plain", "OK");
  });

  server.on("/buzzer", HTTP_GET, [](AsyncWebServerRequest* request) {
    startMelody(ALERT_MELODY, sizeof(ALERT_MELODY)/sizeof(ALERT_MELODY[0]), false);
    request->send(200, "text/plain", "BUZZ");
  });

  server.on("/buzzer/stop", HTTP_GET, [](AsyncWebServerRequest* request) {
    stopMelody();
    request->send(200, "text/plain", "STOP");
  });

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{\"ok\":true,\"ip\":\"" + WiFi.localIP().toString() + "\"}";
    request->send(200, "application/json", json);
  });

  server.on("/stream", HTTP_GET, [](AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = new AsyncJpegStreamResponse();
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QQVGA;
  config.jpeg_quality = 20;
  config.fb_count = 1;
  return esp_camera_init(&config) == ESP_OK;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  servoPan.setPeriodHertz(50); servoTilt.setPeriodHertz(50);
  servoPan.attach(PIN_PAN, 500, 2400); servoTilt.attach(PIN_TILT, 500, 2400);
  servoPan.write(panCurrent); servoTilt.write(tiltCurrent);

  startMelody(STARTUP_MELODY, sizeof(STARTUP_MELODY)/sizeof(STARTUP_MELODY[0]), false);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    startMelody(WIFI_WAIT_BEEP, sizeof(WIFI_WAIT_BEEP)/sizeof(WIFI_WAIT_BEEP[0]), false);
    uint32_t until = millis() + 260;
    while (millis() < until) { updateMelodyNonBlocking(); delay(1); }
    Serial.print('.');
  }
  stopMelody();

  if (!initCamera()) {
    Serial.println("Kamera baslatilamadi");
    while (true) delay(1000);
  }

  registerRoutes();
  server.begin();
}

void loop() {
  updateServosNonBlocking();
  updateMelodyNonBlocking();
}
