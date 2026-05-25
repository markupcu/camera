# ESP32-CAM Pan/Tilt + Web Arayüz (SD Kapalı)

Bu sürüm özellikle **GPIO13/14/15 ile çakışma yaşamamak** için hazırlandı.

## Özellikler
- ESP32-CAM ile web üzerinden canlı yayın,
- Pin 14/15'te pan-tilt servo kontrolü (yumuşak hareket),
- Pin 13'te pasif buzzer (açılış, Wi-Fi bekleme bip'i ve tetikleme melodisi),
- SD kart kaydı **kapalı** (bilinçli tercih: pin çakışması önleme).

## Neden SD kapalı?
ESP32-CAM üzerinde SD_MMC 4-bit modunda GPIO 13/14/15 kullanılır.
Aynı pinlere buzzer + servo bağlandığında kararsızlık/kilitlenme olur.
Bu nedenle bu sürümde SD başlatma ve kayıt kodu kaldırıldı.

## Güç konusunda kritik not
Servo problemi genelde **akım yetersizliğinden** olur. Öneri:
1. Servo beslemesini mümkünse ayrı 5V kaynaktan ver.
2. **GND ortak** olmalı (ESP32 GND ve servo kaynağı GND).
3. Ani akımı azaltmak için kodda adım adım hareket uygulanmıştır.
4. Servo hareketi sırasında gereksiz hız/arttırım yapma.

## Kurulum
1. Arduino IDE'de şu kütüphaneleri kur:
   - `esp32` board package
   - `ESP32Servo`
2. `esp32_cam_pan_tilt.ino` içindeki Wi-Fi bilgilerini gir.
3. Kartı yükle, seri monitörden IP adresini al.
4. Tarayıcıdan `http://<IP_ADRESI>/` aç.
