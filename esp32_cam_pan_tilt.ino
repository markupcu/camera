#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <SD_MMC.h>
#include <FS.h>
#include <time.h>

// ===================== KULLANICI AYARLARI =====================
const char* WIFI_SSID = "WIFI_ADI";
const char* WIFI_PASS = "WIFI_SIFRE";

// Servo pinleri
constexpr int PIN_PAN = 14;
constexpr int PIN_TILT = 15;
constexpr int PIN_BUZZER = 13;

// Servo sınırları (mekanik limitlerine göre düzenle)
constexpr int PAN_MIN = 20;
constexpr int PAN_MAX = 160;
constexpr int TILT_MIN = 30;
constexpr int TILT_MAX = 140;

// Yumuşak hareket ayarları
constexpr int SERVO_STEP_MS = 20;       // küçük ms = daha hızlı/sert
constexpr int SERVO_STEP_DEG = 1;       // küçük derece = daha yumuşak
constexpr int SERVO_HOLD_MS = 100;      // komutlar arası küçük bekleme

// SD kayıt ayarları
constexpr unsigned long FRAME_SAVE_INTERVAL_MS = 900; // ~1 fps

WebServer server(80);
Servo servoPan;
Servo servoTilt;

volatile int panCurrent = 90;
volatile int tiltCurrent = 90;
volatile int panTarget = 90;
volatile int tiltTarget = 90;

unsigned long lastFrameSave = 0;
bool sdReady = false;

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

String nowFileStamp() {
  struct tm t;
  if (!getLocalTime(&t)) {
    return "no_time";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
  return String(buf);
}

void smoothMove(Servo &s, int &current, int target) {
  target = constrain(target, 0, 180);
  if (target == current) return;

  int dir = (target > current) ? 1 : -1;
  while (current != target) {
    current += dir * SERVO_STEP_DEG;
    if ((dir > 0 && current > target) || (dir < 0 && current < target)) {
      current = target;
    }
    s.write(current);
    delay(SERVO_STEP_MS);
  }
}

void moveToTargets() {
  panTarget = constrain(panTarget, PAN_MIN, PAN_MAX);
  tiltTarget = constrain(tiltTarget, TILT_MIN, TILT_MAX);

  smoothMove(servoPan, panCurrent, panTarget);
  delay(SERVO_HOLD_MS);
  smoothMove(servoTilt, tiltCurrent, tiltTarget);
}

void tonePlay(int freq, int ms) {
  ledcWriteTone(0, freq);
  delay(ms);
  ledcWriteTone(0, 0);
  delay(30);
}

void startupMelody() {
  tonePlay(523, 120); // C5
  tonePlay(659, 120); // E5
  tonePlay(784, 150); // G5
  tonePlay(1046, 220); // C6
}

void alertMelody() {
  tonePlay(988, 120);
  tonePlay(880, 120);
  tonePlay(784, 180);
}

void handleRoot() {
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
<h1>Pan / Tilt (Yumuşak Hareket)</h1>
<label>Pan: <span id='panv'>90</span></label><input id='pan' type='range' min='20' max='160' value='90'/>
<label>Tilt: <span id='tiltv'>90</span></label><input id='tilt' type='range' min='30' max='140' value='90'/>
<div class='row'><button class='btn' onclick='savePos()'>Konuma Git</button><button class='btn' onclick='buzz()'>Buzzer Çal</button></div>
<p class='small'>Not: Servo hareketleri küçük adımlarla yapılıyor, düşük adaptörlerde akım pikini azaltır.</p>
</div></div>
<script>
const pan=document.getElementById('pan'), tilt=document.getElementById('tilt');
pan.oninput=()=>document.getElementById('panv').innerText=pan.value;
tilt.oninput=()=>document.getElementById('tiltv').innerText=tilt.value;
async function savePos(){await fetch(`/move?pan=${pan.value}&tilt=${tilt.value}`)}
async function buzz(){await fetch('/buzzer')}
</script></body></html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleMove() {
  if (server.hasArg("pan")) panTarget = server.arg("pan").toInt();
  if (server.hasArg("tilt")) tiltTarget = server.arg("tilt").toInt();
  moveToTargets();
  server.send(200, "text/plain", "OK");
}

void handleBuzzer() {
  alertMelody();
  server.send(200, "text/plain", "BUZZ");
}

void handleStream() {
  WiFiClient client = server.client();
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  while (client.connected()) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) continue;

    server.sendContent("--frame\r\n");
    server.sendContent("Content-Type: image/jpeg\r\n");
    server.sendContent("Content-Length: " + String(fb->len) + "\r\n\r\n");
    client.write(fb->buf, fb->len);
    server.sendContent("\r\n");

    if (sdReady && millis() - lastFrameSave > FRAME_SAVE_INTERVAL_MS) {
      String fn = "/" + nowFileStamp() + ".jpg";
      File f = SD_MMC.open(fn, FILE_WRITE);
      if (f) {
        f.write(fb->buf, fb->len);
        f.close();
      }
      lastFrameSave = millis();
    }

    esp_camera_fb_return(fb);
    delay(15);
  }
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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 14;
  config.fb_count = 2;

  return esp_camera_init(&config) == ESP_OK;
}

void setup() {
  Serial.begin(115200);

  ledcSetup(0, 2000, 8);
  ledcAttachPin(PIN_BUZZER, 0);

  servoPan.setPeriodHertz(50);
  servoTilt.setPeriodHertz(50);
  servoPan.attach(PIN_PAN, 500, 2400);
  servoTilt.attach(PIN_TILT, 500, 2400);
  servoPan.write(panCurrent);
  servoTilt.write(tiltCurrent);

  startupMelody();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.println(WiFi.localIP());

  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  sdReady = SD_MMC.begin();
  if (sdReady) Serial.println("SD kart hazir");

  if (!initCamera()) {
    Serial.println("Kamera baslatilamadi");
    while (true) delay(1000);
  }

  server.on("/", handleRoot);
  server.on("/move", handleMove);
  server.on("/buzzer", handleBuzzer);
  server.on("/stream", HTTP_GET, handleStream);
  server.begin();
}

void loop() {
  server.handleClient();
}
