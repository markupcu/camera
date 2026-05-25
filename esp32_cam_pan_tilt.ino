#include "esp_camera.h"
#include <esp_task_wdt.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <Update.h>

const char* DEFAULT_WIFI_SSID = "WIFI_ADI";
const char* DEFAULT_WIFI_PASS = "WIFI_SIFRE";
const char* AP_SSID = "M3D_Sentry_AP";
const char* AP_PASS = "12345678";

constexpr int PIN_PAN = 14;
constexpr int PIN_TILT = 15;
constexpr int PIN_BUZZER = 13;
constexpr int PIN_LED_FLASH = 4;

#define FLASH_LEDC_CHAN 3
#define BUZZER_LEDC_CHAN 2
constexpr int BUZZER_BASE_FREQ = 2000;
constexpr int BUZZER_LEDC_TIMER_BITS = 8;

constexpr int PAN_MIN = 20, PAN_MAX = 160, TILT_MIN = 30, TILT_MAX = 140;
constexpr uint32_t SERVO_STEP_INTERVAL_MS = 15;
constexpr int SERVO_MAX_STEP = 3;
constexpr uint32_t SCAN_STEP_INTERVAL_MS = 60;
constexpr int SCAN_STEP_DEG = 2;
#define WDT_TIMEOUT_SECONDS 8
constexpr int PRESET_COUNT = 5;

struct Note { int freq; uint16_t ms; };
const Note STARTUP_MELODY[] PROGMEM = {{523,100},{659,100},{784,120},{1046,200}};
const Note ALERT_MELODY[] PROGMEM = {{988,90},{1318,90},{1568,110},{0,35},{1568,130},{0,35},{1318,90}};
const Note PRESET_SAVED_MELODY[] PROGMEM = {{880,80},{1100,120}};
const Note WIFI_WAIT_BEEP[] PROGMEM = {{440,30},{0,150}};

AsyncWebServer server(80);
Preferences prefs;
Servo servoPan, servoTilt;
int panCurrent=90, tiltCurrent=90, panTarget=90, tiltTarget=90;
uint32_t lastServoStepMs=0;

const Note* activeMelody=nullptr; size_t melodyLen=0, melodyIdx=0; uint32_t melodyNextMs=0; bool melodyLoop=false, buzzerAttached=false;
int flashBrightness=0; bool nightMode=false;
bool scanActive=false; int scanDir=1; uint32_t lastScanMs=0;
int presets[PRESET_COUNT][2];
uint32_t bootTime=0; bool isAPMode=false;

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

void ensureBuzzerAttached(){ if(!buzzerAttached){ ledcAttachChannel(PIN_BUZZER,BUZZER_BASE_FREQ,BUZZER_LEDC_TIMER_BITS,BUZZER_LEDC_CHAN); buzzerAttached=true; } }
void buzzerOff(){ if(buzzerAttached){ ledcWriteTone(PIN_BUZZER,0); ledcDetach(PIN_BUZZER); buzzerAttached=false; } pinMode(PIN_BUZZER,OUTPUT); digitalWrite(PIN_BUZZER,LOW); }
void startMelody(const Note* notes,size_t len,bool loopPlayback=false){ activeMelody=notes; melodyLen=len; melodyIdx=0; melodyNextMs=0; melodyLoop=loopPlayback; }
void stopMelody(){ activeMelody=nullptr; melodyLen=0; melodyIdx=0; melodyLoop=false; buzzerOff(); }
void updateMelodyNonBlocking(){ if(!activeMelody||melodyLen==0) return; uint32_t now=millis(); if(now<melodyNextMs)return; Note n; memcpy_P(&n,&activeMelody[melodyIdx],sizeof(Note)); if(n.freq<=0)buzzerOff(); else { ensureBuzzerAttached(); ledcWriteTone(PIN_BUZZER,n.freq);} melodyNextMs=now+n.ms; if(++melodyIdx>=melodyLen){ if(melodyLoop) melodyIdx=0; else stopMelody(); }}

void updateServosNonBlocking(){ uint32_t now=millis(); if(now-lastServoStepMs<SERVO_STEP_INTERVAL_MS)return; lastServoStepMs=now; panTarget=constrain(panTarget,PAN_MIN,PAN_MAX); tiltTarget=constrain(tiltTarget,TILT_MIN,TILT_MAX); auto nxt=[](int c,int t){int d=t-c; if(abs(d)<=1)return t; int s=constrain(abs(d)/4,1,SERVO_MAX_STEP); return c<t?c+s:c-s;}; panCurrent=nxt(panCurrent,panTarget); tiltCurrent=nxt(tiltCurrent,tiltTarget); servoPan.write(panCurrent); servoTilt.write(tiltCurrent); }
void updateScanNonBlocking(){ if(!scanActive)return; uint32_t now=millis(); if(now-lastScanMs<SCAN_STEP_INTERVAL_MS)return; lastScanMs=now; panTarget+=scanDir*SCAN_STEP_DEG; if(panTarget>=PAN_MAX){panTarget=PAN_MAX;scanDir=-1;} if(panTarget<=PAN_MIN){panTarget=PAN_MIN;scanDir=1;} }

void initFlashPWM(){ ledcAttachChannel(PIN_LED_FLASH,5000,8,FLASH_LEDC_CHAN); ledcWrite(PIN_LED_FLASH,0);} 
void setFlashBrightness(int v){ flashBrightness=constrain(v,0,255); ledcWrite(PIN_LED_FLASH,flashBrightness);} 
void loadPresets(){ prefs.begin("presets",true); for(int i=0;i<PRESET_COUNT;i++){ presets[i][0]=prefs.getInt(("p"+String(i)+"a").c_str(),90); presets[i][1]=prefs.getInt(("p"+String(i)+"b").c_str(),90);} prefs.end(); }
void savePreset(int idx,int pan,int tilt){ if(idx<0||idx>=PRESET_COUNT)return; presets[idx][0]=pan; presets[idx][1]=tilt; prefs.begin("presets",false); prefs.putInt(("p"+String(idx)+"a").c_str(),pan); prefs.putInt(("p"+String(idx)+"b").c_str(),tilt); prefs.end(); }

class AsyncJpegStreamResponse: public AsyncAbstractResponse{ camera_fb_t* _fb=nullptr; size_t _index=0,_hdrLen=0; uint8_t _hdrBuf[96]; public: AsyncJpegStreamResponse(){_code=200;_contentType="multipart/x-mixed-replace; boundary=frame";_sendContentLength=false;_chunked=true;} ~AsyncJpegStreamResponse(){ if(_fb) esp_camera_fb_return(_fb);} bool _sourceValid() const override{return true;} size_t _fillBuffer(uint8_t* buf,size_t maxLen) override { if(!_fb){_fb=esp_camera_fb_get(); if(!_fb) return RESPONSE_TRY_AGAIN; _index=0; String h="--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "+String(_fb->len)+"\r\n\r\n"; _hdrLen=h.length(); if(_hdrLen>sizeof(_hdrBuf)){esp_camera_fb_return(_fb);_fb=nullptr; return RESPONSE_TRY_AGAIN;} memcpy(_hdrBuf,h.c_str(),_hdrLen);} if(_index<_hdrLen){ size_t n=min(maxLen,_hdrLen-_index); memcpy(buf,_hdrBuf+_index,n); _index+=n; return n;} size_t fbIdx=_index-_hdrLen; if(fbIdx<_fb->len){ size_t n=min(maxLen,_fb->len-fbIdx); memcpy(buf,_fb->buf+fbIdx,n); _index+=n; return n;} size_t tailIdx=_index-_hdrLen-_fb->len; if(tailIdx<2){ size_t n=min(maxLen,(size_t)(2-tailIdx)); if(n>=1)buf[0]='\r'; if(n>=2)buf[1]='\n'; _index+=n; return n;} esp_camera_fb_return(_fb); _fb=nullptr; _index=0; _hdrLen=0; delay(1); return RESPONSE_TRY_AGAIN; }};

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang='tr'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>M3D Sentry</title></head><body style='background:#0b1220;color:#dbeafe;font-family:Arial'>
<h2>M3D Sentry</h2><img id='cam' src='/stream' style='max-width:100%'><br>
Pan <input id='pan' type='range' min='20' max='160' value='90'> Tilt <input id='tilt' type='range' min='30' max='140' value='90'>
<button onclick='fetch(`/move?pan=${pan.value}&tilt=${tilt.value}`)'>Move</button>
<button onclick='fetch(`/scan/start`)'>Scan On</button><button onclick='fetch(`/scan/stop`)'>Scan Off</button>
<button onclick='fetch(`/buzzer`)'>Siren</button><button onclick='fetch(`/buzzer/stop`)'>Stop</button>
</body></html>)HTML";

void registerRoutes(){
server.on("/",HTTP_GET,[](AsyncWebServerRequest* r){r->send_P(200,"text/html",INDEX_HTML);});
server.on("/move",HTTP_GET,[](AsyncWebServerRequest* r){ if(r->hasParam("pan"))panTarget=constrain(r->getParam("pan")->value().toInt(),PAN_MIN,PAN_MAX); if(r->hasParam("tilt"))tiltTarget=constrain(r->getParam("tilt")->value().toInt(),TILT_MIN,TILT_MAX); r->send(200,"text/plain","OK");});
server.on("/preset/save",HTTP_GET,[](AsyncWebServerRequest* r){ if(!r->hasParam("idx")){r->send(400,"text/plain","ERR");return;} int idx=r->getParam("idx")->value().toInt(); int pan=r->hasParam("pan")?r->getParam("pan")->value().toInt():panCurrent; int tilt=r->hasParam("tilt")?r->getParam("tilt")->value().toInt():tiltCurrent; savePreset(idx,pan,tilt); startMelody(PRESET_SAVED_MELODY,sizeof(PRESET_SAVED_MELODY)/sizeof(PRESET_SAVED_MELODY[0]),false); r->send(200,"text/plain","SAVED");});
server.on("/preset/go",HTTP_GET,[](AsyncWebServerRequest* r){ if(!r->hasParam("idx")){r->send(400,"text/plain","ERR");return;} int idx=r->getParam("idx")->value().toInt(); if(idx<0||idx>=PRESET_COUNT){r->send(400,"text/plain","ERR");return;} panTarget=presets[idx][0]; tiltTarget=presets[idx][1]; r->send(200,"text/plain","GO");});
server.on("/scan/start",HTTP_GET,[](AsyncWebServerRequest* r){scanActive=true; scanDir=1; r->send(200,"text/plain","SCAN_ON");});
server.on("/scan/stop",HTTP_GET,[](AsyncWebServerRequest* r){scanActive=false; r->send(200,"text/plain","SCAN_OFF");});
server.on("/flash/set",HTTP_GET,[](AsyncWebServerRequest* r){ if(r->hasParam("val")){int v=r->getParam("val")->value().toInt(); setFlashBrightness(v); nightMode=(v>0);} r->send(200,"text/plain","OK");});
server.on("/night",HTTP_GET,[](AsyncWebServerRequest* r){ nightMode=r->hasParam("on")&&r->getParam("on")->value()=="1"; setFlashBrightness(nightMode?255:0); r->send(200,"text/plain",nightMode?"NIGHT_ON":"NIGHT_OFF");});
server.on("/buzzer",HTTP_GET,[](AsyncWebServerRequest* r){ startMelody(ALERT_MELODY,sizeof(ALERT_MELODY)/sizeof(ALERT_MELODY[0]),false); r->send(200,"text/plain","BUZZ");});
server.on("/buzzer/stop",HTTP_GET,[](AsyncWebServerRequest* r){ stopMelody(); r->send(200,"text/plain","STOP");});
server.on("/camera/quality",HTTP_GET,[](AsyncWebServerRequest* r){ if(!r->hasParam("q")){r->send(400,"text/plain","ERR");return;} auto s=esp_camera_sensor_get(); if(s) s->set_quality(s,constrain(r->getParam("q")->value().toInt(),4,63)); r->send(200,"text/plain","OK");});
server.on("/camera/framesize",HTTP_GET,[](AsyncWebServerRequest* r){ if(!r->hasParam("size")){r->send(400,"text/plain","ERR");return;} auto s=esp_camera_sensor_get(); if(!s){r->send(500,"text/plain","ERR");return;} String sz=r->getParam("size")->value(); framesize_t fs=FRAMESIZE_QVGA; if(sz=="QQVGA")fs=FRAMESIZE_QQVGA; else if(sz=="VGA")fs=FRAMESIZE_VGA; else if(sz=="SVGA")fs=FRAMESIZE_SVGA; s->set_framesize(s,fs); r->send(200,"text/plain","OK");});
server.on("/wifi/save",HTTP_GET,[](AsyncWebServerRequest* r){ if(r->hasParam("ssid")&&r->hasParam("pass")){ String ssid=r->getParam("ssid")->value(),pass=r->getParam("pass")->value(); prefs.begin("wifi_cfg",false); prefs.putString("ssid",ssid); prefs.putString("pass",pass); prefs.end(); r->send(200,"text/plain","OK_RESTARTING"); delay(1500); ESP.restart(); } else r->send(400,"text/plain","MISSING_PARAMS");});
server.on("/update",HTTP_POST,[](AsyncWebServerRequest *r){ bool ok=!Update.hasError(); auto *resp=r->beginResponse(ok?200:500,"text/plain",ok?"OK":"FAIL"); resp->addHeader("Connection","close"); r->send(resp); delay(1200); ESP.restart();},[](AsyncWebServerRequest*,String filename,size_t index,uint8_t *data,size_t len,bool final){ if(!index){ if(!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);} if(!Update.hasError()) if(Update.write(data,len)!=len) Update.printError(Serial); if(final) Update.end(true);});
server.on("/status",HTTP_GET,[](AsyncWebServerRequest* r){ uint32_t up=(millis()-bootTime)/1000; String j="{"; j+="\"ok\":true,"; j+="\"ip\":\""+(isAPMode?WiFi.softAPIP().toString():WiFi.localIP().toString())+"\","; j+="\"pan\":"+String(panCurrent)+","; j+="\"tilt\":"+String(tiltCurrent)+","; j+="\"scanActive\":"+String(scanActive?"true":"false")+","; j+="\"nightMode\":"+String(nightMode?"true":"false")+","; j+="\"apMode\":"+String(isAPMode?"true":"false")+","; j+="\"uptimeSec\":"+String(up)+","; j+="\"freeHeap\":"+String(ESP.getFreeHeap())+"}"; r->send(200,"application/json",j);});
server.on("/stream",HTTP_GET,[](AsyncWebServerRequest* r){ auto* resp=new AsyncJpegStreamResponse(); resp->addHeader("Access-Control-Allow-Origin","*"); r->send(resp);});
}

bool initCamera(){ camera_config_t c; c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0; c.pin_d0=Y2_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d3=Y5_GPIO_NUM; c.pin_d4=Y6_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d7=Y9_GPIO_NUM; c.pin_xclk=XCLK_GPIO_NUM; c.pin_pclk=PCLK_GPIO_NUM; c.pin_vsync=VSYNC_GPIO_NUM; c.pin_href=HREF_GPIO_NUM; c.pin_sccb_sda=SIOD_GPIO_NUM; c.pin_sccb_scl=SIOC_GPIO_NUM; c.pin_pwdn=PWDN_GPIO_NUM; c.pin_reset=RESET_GPIO_NUM; c.xclk_freq_hz=20000000; c.pixel_format=PIXFORMAT_JPEG; c.grab_mode=CAMERA_GRAB_LATEST; if(psramFound()){ c.frame_size=FRAMESIZE_QVGA; c.jpeg_quality=14; c.fb_count=2; c.fb_location=CAMERA_FB_IN_PSRAM; } else { c.frame_size=FRAMESIZE_QQVGA; c.jpeg_quality=20; c.fb_count=1; c.fb_location=CAMERA_FB_IN_DRAM; } return esp_camera_init(&c)==ESP_OK; }

void setup(){ Serial.begin(115200); initFlashPWM(); pinMode(PIN_BUZZER,OUTPUT); digitalWrite(PIN_BUZZER,LOW); servoPan.setPeriodHertz(50); servoTilt.setPeriodHertz(50); servoPan.attach(PIN_PAN,500,2400); servoTilt.attach(PIN_TILT,500,2400); servoPan.write(panCurrent); servoTilt.write(tiltCurrent); loadPresets(); startMelody(STARTUP_MELODY,sizeof(STARTUP_MELODY)/sizeof(STARTUP_MELODY[0]),false);
prefs.begin("wifi_cfg",true); String ssid=prefs.getString("ssid",DEFAULT_WIFI_SSID), pass=prefs.getString("pass",DEFAULT_WIFI_PASS); prefs.end(); WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(),pass.c_str()); uint32_t st=millis(); bool connected=true; while(WiFi.status()!=WL_CONNECTED){ startMelody(WIFI_WAIT_BEEP,sizeof(WIFI_WAIT_BEEP)/sizeof(WIFI_WAIT_BEEP[0]),false); uint32_t until=millis()+260; while(millis()<until){ updateMelodyNonBlocking(); updateServosNonBlocking(); delay(1);} if(millis()-st>15000){connected=false;break;}}
if(connected){ stopMelody(); WiFi.setSleep(false); isAPMode=false; if(MDNS.begin("m3dsentry")){} } else { WiFi.disconnect(); WiFi.mode(WIFI_AP); WiFi.softAP(AP_SSID,AP_PASS); isAPMode=true; }
if(!initCamera()){ while(true){delay(500);} }
registerRoutes(); server.begin(); esp_task_wdt_config_t cfg={.timeout_ms=WDT_TIMEOUT_SECONDS*1000,.idle_core_mask=(1<<0)|(1<<1),.trigger_panic=true}; esp_task_wdt_init(&cfg); esp_task_wdt_add(NULL); bootTime=millis(); }

void loop(){ updateServosNonBlocking(); updateMelodyNonBlocking(); updateScanNonBlocking(); esp_task_wdt_reset(); }
