# Mariss3D ESP32-CAM Gözetleme Kulesi (SD Kapalı)

Bu sürüm **stabilite + görsellik + sesli etkileşim** odaklı, non-blocking mimariyle hazırlandı.

## Öne Çıkanlar
- 🎥 Akıcı async MJPEG canlı yayın (`/stream`)
- 🎛️ Pan/Tilt kontrol (GPIO14/15) non-blocking adımlı servo sürüş
- 🔊 Buzzer etkileşimi (GPIO13):
  - açılış melodisi
  - Wi-Fi bekleme bip’i
  - panelden “Korna Çal” + “Sesi Durdur”
- 🩺 Sağlık endpoint’i (`/health`) ve panelde canlı durum rozeti
- 💡 SD kart bilinçli kapalı (GPIO13/14/15 çakışması önlendi)

## Mimari Notlar
- `/move` endpoint’i sadece hedef açıları günceller.
- Servo hareketi `loop()` içinde `updateServosNonBlocking()` ile akar.
- Buzzer da `delay` kullanmadan `updateMelodyNonBlocking()` ile yürütülür.
- Bu sayede yayın devam ederken kontrol ve ses komutları anında cevap verir.

## Gerekli Kütüphaneler
- `esp32` board package
- `ESP32Servo`
- `ESPAsyncWebServer`
- `AsyncTCP`

## Kurulum
1. `WIFI_SSID` ve `WIFI_PASS` alanlarını doldur.
2. Kart tipini AI Thinker ESP32-CAM seç.
3. Sketch'i yükle.
4. Seri portta görünen IP adresini tarayıcıda aç.
