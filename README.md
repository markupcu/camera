# Mariss3D Gözetleme Kulesi — Endüstriyel Sürüm (PIR Çıkarıldı)

Bu sürümde PIR sensörü akıştan çıkarıldı ve sistem çekirdeği pan/tilt + akış + OTA + ağ dayanıklılığına odaklandı.

## Özellikler
- Async MJPEG canlı yayın (`/stream`)
- Non-blocking pan/tilt kontrol + tarama modu
- Buzzer/siren kontrolü (`/buzzer`, `/buzzer/stop`)
- Preset kaydetme/çağırma
- Flash parlaklık + gece modu
- Wi-Fi STA + başarısız olursa SoftAP fallback
- OTA güncelleme (`/update`)
- Durum endpoint'i (`/status`)

## PIR durumu
- PIR pin/algılama mantığı tamamen kaldırıldı.
- UI/JSON artık PIR alanı döndürmez.

## Kurulum
- Gerekli kütüphaneler: `ESPAsyncWebServer`, `AsyncTCP`, `ESP32Servo`, `Preferences`, `Update`
- AI Thinker ESP32-CAM hedefiyle derleyin.
