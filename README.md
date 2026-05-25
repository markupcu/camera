# ESP32-CAM Pan/Tilt + Web Arayüz + SD Kayıt

Bu örnek:
- ESP32-CAM ile web üzerinden canlı yayın,
- Pin 14/15'te pan-tilt servo kontrolü (yumuşak hareket),
- Pin 13'te pasif buzzer (açılış ve tetikleme melodisi),
- SD karta zaman damgalı JPEG kayıt (düşük hızda, sistemi zorlamadan)
özelliklerini içerir.

## Güç konusunda kritik not
Servo problemi genelde **akım yetersizliğinden** olur. Öneri:
1. Servo beslemesini mümkünse ayrı 5V kaynaktan ver.
2. **GND ortak** olmalı (ESP32 GND ve servo kaynağı GND).
3. Ani akımı azaltmak için kodda adım adım hareket uygulanmıştır.
4. Kamera çözünürlüğünü yüksek tutma; gerektiğinde `FRAMESIZE_QVGA` kullan.

## Kurulum
1. Arduino IDE'de şu kütüphaneleri kur:
   - `esp32` board package
   - `ESP32Servo`
2. `esp32_cam_pan_tilt.ino` içindeki Wi-Fi bilgilerini gir.
3. Kartı yükle, seri monitörden IP adresini al.
4. Tarayıcıdan `http://<IP_ADRESI>/` aç.

## Notlar
- SD kayıt `FRAME_SAVE_INTERVAL_MS` ile sınırlandı (~1fps).
- Daha stabil güç için servo hızını artırma (adım sürelerini düşürme).
- Pasif buzzer için farklı melodi fonksiyonları ekleyebilirsin.
