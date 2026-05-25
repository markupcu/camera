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

AsyncWebServer server(80);
Servo servoPan;
Servo servoTilt;

volatile int panCurrent = 90;
volatile int tiltCurrent = 90;
volatile int panTarget = 90;
volatile int tiltTarget = 90;
uint32_t lastServoStepMs = 0;

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

void tonePlay(int freq, int ms) {
  ledcWriteTone(BUZZER_LEDC_CHANNEL, freq);
  delay(ms);
  ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
  delay(30);
}

void wifiWaitingBeep() {
  tonePlay(440, 40);
  delay(140);
}

void startupMelody() {
  tonePlay(523, 120);
  tonePlay(659, 120);
  tonePlay(784, 150);
  tonePlay(1046, 220);
}

void alertMelody() {
  tonePlay(988, 120);
  tonePlay(880, 120);
  tonePlay(784, 180);
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

void registerRoutes() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
<!doctype html><html lang='tr'><head><meta charset='utf-8'/>
<meta name='viewport' content='width=device-width,initial-scale=1'/>
<title>ESP32-CAM Pan/Tilt Kontrol</title>
<style>
body{font-family:Inter,Arial,sans-serif;background:#0f172a;color:#e2e8f0;margin:0;padding:0}
.wrap{max-width:980px;margin:24px auto;padding:16px}
.card{background:#111827;border:1px solid #1f2937;border-radius:16px;padding:16px;margin-bottom:16px;box-shadow:0 10px 30px rgba(0,0,0,.25)}
img{width:100%;border-radius:12px;border:1px solid #334155}
h1{font-size:1.2rem;margin:0 0 10px}
.row{display:flex;gap:12px;flex-wrap:wrap}
.btn{background:#1d4ed8;border:none;padding:10px 14px;border-radius:10px;color:#fff;cursor:pointer}
.btn:hover{filter:brightness(1.1)}
label{display:block;margin:8px 0 4px}
input[type=range]{width:100%}
.small{font-size:.9rem;color:#94a3b8}
</style></head>
<body><div class='wrap'>
<div class='card'><h1>Canlı Yayın</h1><img src='/stream'/></div>
<div class='card'>
<h1>Pan / Tilt (Akıcı Non-Blocking)</h1>
<label>Pan: <span id='panv'>90</span></label><input id='pan' type='range' min='20' max='160' value='90'/>
<label>Tilt: <span id='tiltv'>90</span></label><input id='tilt' type='range' min='30' max='140' value='90'/>
<div class='row'><button class='btn' onclick='savePos()'>Konuma Git</button><button class='btn' onclick='buzz()'>Buzzer Çal</button></div>
<p class='small'>Servo hedefleri hemen set edilir; hareket loop içinde adım adım, kilitlemeden yapılır.</p>
</div></div>
<script>
const pan=document.getElementById('pan'), tilt=document.getElementById('tilt');
pan.oninput=()=>document.getElementById('panv').innerText=pan.value;
tilt.oninput=()=>document.getElementById('tiltv').innerText=tilt.value;
async function savePos(){await fetch(`/move?pan=${pan.value}&tilt=${tilt.value}`)}
async function buzz(){await fetch('/buzzer')}
</script></body></html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  server.on("/move", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("pan")) panTarget = request->getParam("pan")->value().toInt();
    if (request->hasParam("tilt")) tiltTarget = request->getParam("tilt")->value().toInt();
    request->send(200, "text/plain", "OK");
  });

  server.on("/buzzer", HTTP_GET, [](AsyncWebServerRequest *request){
    alertMelody();
    request->send(200, "text/plain", "BUZZ");
  });

  server.on("/stream", HTTP_GET, [](AsyncWebServerRequest *request){
    AsyncWebServerResponse *response = request->beginChunkedResponse(
      "multipart/x-mixed-replace; boundary=frame",
      [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        (void)index;
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) return 0;

        String head = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + String(fb->len) + "\r\n\r\n";
        size_t hlen = head.length();
        size_t total = hlen + fb->len + 2;
        if (maxLen < total) {
          esp_camera_fb_return(fb);
          return 0;
        }

        memcpy(buffer, head.c_str(), hlen);
        memcpy(buffer + hlen, fb->buf, fb->len);
        buffer[hlen + fb->len] = '\r';
        buffer[hlen + fb->len + 1] = '\n';

        esp_camera_fb_return(fb);
        return total;
      }
    );
    response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
  });
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 16;
  config.fb_count = 2;
  return esp_camera_init(&config) == ESP_OK;
}

void setup() {
  Serial.begin(115200);

  ledcSetup(BUZZER_LEDC_CHANNEL, BUZZER_BASE_FREQ, BUZZER_LEDC_TIMER_BITS);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);

  servoPan.setPeriodHertz(50);
  servoTilt.setPeriodHertz(50);
  servoPan.attach(PIN_PAN, 500, 2400);
  servoTilt.attach(PIN_TILT, 500, 2400);
  servoPan.write(panCurrent);
  servoTilt.write(tiltCurrent);

  startupMelody();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    wifiWaitingBeep();
  }
  Serial.println();
  Serial.println(WiFi.localIP());

  if (!initCamera()) {
    Serial.println("Kamera baslatilamadi");
    while (true) delay(1000);
  }

  registerRoutes();
  server.begin();
}

void loop() {
  updateServosNonBlocking();
}
