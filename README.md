# ESP32-CAM Pan/Tilt + Async Web Arayüz (SD Kapalı)

Bu sürümde iki kilitlenme sebebi birlikte çözüldü:
1. `WebServer` + sürekli `/stream` bağlantısının diğer endpoint'leri bloklaması,
2. Servo hareketinde blocking `delay` döngülerinin kontrol akışını kilitlemesi.

## Özellikler
- ESP32-CAM canlı akış (`/stream`)
- Async web kontrol paneli (`/`, `/move`, `/buzzer`)
- GPIO14/15 pan-tilt servo (non-blocking adımlı hareket)
- GPIO13 pasif buzzer (açılış + Wi-Fi bekleme + manuel tetikleme)
- SD kart **kapalı** (GPIO13/14/15 çakışmasını önlemek için)

## Gerekli Kütüphaneler
- `esp32` board package
- `ESP32Servo`
- `ESPAsyncWebServer`
- `AsyncTCP`

## Neden bu sürüm daha stabil?
- `/move` artık sadece hedef açıları set eder, servo hareketi `loop()` içinde küçük adımlarla akar.
- `/stream` async çalıştığı için görüntü akarken bile `/move` ve `/buzzer` yanıt verebilir.
- Kamera için QVGA + daha düşük kalite ayarı ile CPU/RAM yükü azaltıldı.
