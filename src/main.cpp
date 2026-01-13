#define BLYNK_TEMPLATE_ID "BLYNK_TEMPLATE_ID" // Kendi template ID'nizi buraya ekleyin
#define BLYNK_TEMPLATE_NAME "Çevre Aydınlatma ESP32"
#define BLYNK_AUTH_TOKEN  "BLYNK_AUTH_TOKEN" // Kendi auth token'inizi buraya ekleyin

// Blynk optimizasyonları
#define BLYNK_PRINT Serial
#define BLYNK_HEARTBEAT 45  // Varsayılan 10sn yerine 45sn (daha az veri)

#include <BlynkSimpleEsp32.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <time.h>

// WiFi bilgileri
char ssid[] = "ssid"; // Kendi SSID'nizi buraya ekleyin
char pass[] = "pass"; // Kendi şifrenizi buraya ekleyin

// Pin tanımlamaları
#define RELAY1  26
// #define RELAY2  27  // İkinci röle için hazır
#define BUTTON  14

// Durum değişkenleri
bool relay1State = false;
int lastStableButtonState = HIGH; // Pull-up kullanıldığı için HIGH (buton basılı değilken)
int currentButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// Zamanlama
unsigned long lastCheck = 0;
const unsigned long checkInterval = 60000; // 60 sn'de bir kontrol
unsigned long bootTime;  
const unsigned long resetInterval = 86400000; // 24 saat
const unsigned long offlineResetInterval = 3600000; // 1 saat (offline mod için)
unsigned long lastOnlineTime = 0; // Son çevrimiçi olma zamanı

// WiFi güç tasarrufu
const int wifiChannel = 1; // Sabit kanal kullan, tarama yapma
const long wifiTimeout = 10000; // WiFi bağlantı timeout'u
const int maxRetries = 5; // Maksimum deneme sayısı

// Zamanlayıcı değişkenleri
long scheduleOnSeconds = -1;    // Açılış zamanı (gece yarısından itibaren saniye)
long scheduleOffSeconds = -1;   // Kapanış zamanı (gece yarısından itibaren saniye)
bool scheduleEnabled = false;   // Zamanlayıcı aktif mi? (V6)
int lastExecutedMinute = -1;    // Son çalıştırılan dakika (tekrar kontrolü için)

// NTP sunucuları (Türkiye saati için)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 10800; // GMT+3 (Türkiye)
const int daylightOffset_sec = 0; // Yaz saati uygulaması yok

// Blynk'ten gelen V1 komutu (röle kontrolü)
BLYNK_WRITE(V1) {
  int value = param.asInt();
  relay1State = (value == 1);
  digitalWrite(RELAY1, relay1State ? HIGH : LOW);
  Serial.printf("Blynk'ten manuel komut: %d\n", value);
  
  // Zamanlayıcı aktifse uyarı ver, bir sonraki kontrolde zamanlayıcı devreye girecek
  if (scheduleEnabled) {
    Serial.println("UYARI: Manuel müdahale yapıldı, ancak zamanlayıcı hala aktif!");
    Serial.println("Zamanlayıcı bir sonraki kontrolde kurallarını uygulayacak.");
    // lastExecutedMinute'i sıfırla ki hemen kontrol etsin
    lastExecutedMinute = -1;
  }
}

// V2: Açılış zamanı (Time Input)
BLYNK_WRITE(V2) {
  TimeInputParam t(param);
  
  if (t.hasStartTime()) {
    scheduleOnSeconds = t.getStartHour() * 3600 + t.getStartMinute() * 60;
    Serial.printf("Açılış zamanı ayarlandı: %02d:%02d\n", t.getStartHour(), t.getStartMinute());
  } else {
    scheduleOnSeconds = -1;
    Serial.println("Açılış zamanı temizlendi");
  }
}

// V3: Kapanış zamanı (Time Input)
BLYNK_WRITE(V3) {
  TimeInputParam t(param);
  
  if (t.hasStartTime()) {
    scheduleOffSeconds = t.getStartHour() * 3600 + t.getStartMinute() * 60;
    Serial.printf("Kapanış zamanı ayarlandı: %02d:%02d\n", t.getStartHour(), t.getStartMinute());
  } else {
    scheduleOffSeconds = -1;
    Serial.println("Kapanış zamanı temizlendi");
  }
}

// V6: Zamanlayıcı aktif/pasif (Switch: 1=aktif, 0=pasif)
BLYNK_WRITE(V6) {
  scheduleEnabled = (param.asInt() == 1);
  Serial.printf("Zamanlayıcı %s\n", scheduleEnabled ? "AKTİF" : "PASİF");
  if (scheduleEnabled) {
    lastExecutedMinute = -1; // Yeniden aktifleştirildiğinde sıfırla
  }
}

// Blynk bağlantı durumu
BLYNK_CONNECTED() {
  Serial.println("Blynk'e bağlandı!");
  Blynk.syncVirtual(V1, V2, V3, V6); // Tüm ayarları senkronize et
  lastOnlineTime = millis(); // Çevrimiçi zaman damgasını güncelle
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== ESP32 Röle Kontrolü Başlatılıyor ===");

  // Pin ayarları - Röle LOW'dan başlar
  pinMode(RELAY1, OUTPUT);
  digitalWrite(RELAY1, LOW);
  relay1State = false;
  pinMode(BUTTON, INPUT_PULLUP); // ESP32 dahili pull-up direnci kullanılıyor

  // WiFi optimize ayarları
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true); // Light sleep
  WiFi.setAutoReconnect(true);
  
  // WiFi bağlantısı
  Serial.print("WiFi'ye bağlanılıyor");
  int wifiRetryCount = 0;
  
  while (wifiRetryCount < maxRetries) {
    WiFi.begin(ssid, pass);
    
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < wifiTimeout) {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi bağlantısı başarılı!");
      Serial.print("IP Adresi: ");
      Serial.println(WiFi.localIP());
      
      // NTP ile zaman senkronizasyonu
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      Serial.println("Zaman senkronizasyonu başlatıldı...");
      
      break;
    } else {
      wifiRetryCount++;
      Serial.printf("\nWiFi bağlantısı başarısız! Deneme: %d/%d\n", wifiRetryCount, maxRetries);
      
      if (wifiRetryCount < maxRetries) {
        Serial.println("2 saniye sonra tekrar denenecek...");
        WiFi.disconnect();
        delay(2000);
      }
    }
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi'ye bağlanılamadı, offline modda devam ediliyor...");
    Serial.println("Offline modda zamanlayıcı çalışmayacak, sadece buton kontrolü aktif.");
  }

  // Blynk bağlantısı
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.config(BLYNK_AUTH_TOKEN);
    
    int blynkRetryCount = 0;
    while (blynkRetryCount < maxRetries && !Blynk.connected()) {
      Serial.printf("Blynk'e bağlanılıyor... Deneme: %d/%d\n", blynkRetryCount + 1, maxRetries);
      
      if (Blynk.connect(3000)) {
        Serial.println("Blynk bağlantısı başarılı!");
        lastOnlineTime = millis();
        break;
      } else {
        blynkRetryCount++;
        if (blynkRetryCount < maxRetries) {
          Serial.println("2 saniye sonra tekrar denenecek...");
          delay(2000);
        }
      }
    }
    
    if (!Blynk.connected()) {
      Serial.println("Blynk'e bağlanılamadı, sadece WiFi ile devam ediliyor...");
    }
  }

  bootTime = millis();
  Serial.println("=== Sistem Hazır ===");
  Serial.println("Röle başlangıç durumu: KAPALI\n");
}

// Zamanlayıcı kontrolü (sadece online modda çalışır)
void checkSchedule() {
  // Zamanlayıcı kapalı, Blynk bağlı değil veya WiFi yok ise kontrol etme
  if (!scheduleEnabled || !Blynk.connected() || WiFi.status() != WL_CONNECTED) {
    return;
  }

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return; // Zaman bilgisi alınamazsa kontrol etme
  }

  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int currentSecond = timeinfo.tm_sec;
  
  // Şu anki zamanı saniyeye çevir (gece yarısından itibaren)
  long currentTimeSeconds = currentHour * 3600 + currentMinute * 60 + currentSecond;
  
  // Dakika değişimini kontrol et (aynı dakikada tekrar çalışmasın)
  int currentMinuteOfDay = currentHour * 60 + currentMinute;
  
  // Zamanlayıcı aralığında mı kontrol et
  bool shouldBeOn = false;
  
  if (scheduleOnSeconds >= 0 && scheduleOffSeconds >= 0) {
    // Hem açılış hem kapanış zamanı ayarlanmış
    if (scheduleOnSeconds < scheduleOffSeconds) {
      // Normal durum: örn. 18:00 - 22:00
      shouldBeOn = (currentTimeSeconds >= scheduleOnSeconds && currentTimeSeconds < scheduleOffSeconds);
    } else {
      // Gece yarısını geçen durum: örn. 22:00 - 06:00
      shouldBeOn = (currentTimeSeconds >= scheduleOnSeconds || currentTimeSeconds < scheduleOffSeconds);
    }
  } else if (scheduleOnSeconds >= 0) {
    // Sadece açılış zamanı var
    shouldBeOn = (currentTimeSeconds >= scheduleOnSeconds);
  } else if (scheduleOffSeconds >= 0) {
    // Sadece kapanış zamanı var
    shouldBeOn = (currentTimeSeconds < scheduleOffSeconds);
  }
  
  // Röle durumunu zamanlayıcıya göre ayarla
  if (shouldBeOn && !relay1State) {
    // Açık olması gerekiyor ama kapalı
    relay1State = true;
    digitalWrite(RELAY1, HIGH);
    Blynk.virtualWrite(V1, 1);
    Serial.printf("Zamanlayıcı: Röle AÇILDI (%02d:%02d) - Zaman aralığında\n", currentHour, currentMinute);
    lastExecutedMinute = currentMinuteOfDay;
  } else if (!shouldBeOn && relay1State) {
    // Kapalı olması gerekiyor ama açık
    relay1State = false;
    digitalWrite(RELAY1, LOW);
    Blynk.virtualWrite(V1, 0);
    Serial.printf("Zamanlayıcı: Röle KAPANDI (%02d:%02d) - Zaman aralığı dışında\n", currentHour, currentMinute);
    lastExecutedMinute = currentMinuteOfDay;
  }
  
  // Dakika değiştiğinde sıfırla
  if (currentMinuteOfDay != lastExecutedMinute && lastExecutedMinute != -1) {
    // Yeni dakikaya geçildi, bir sonraki kontrol için hazır ol
  }
}

void loop() {
  // Blynk bağlıysa çalıştır
  if (Blynk.connected()) {
    Blynk.run();
    lastOnlineTime = millis(); // Çevrimiçi olduğu sürece güncelle
  }

  // Zamanlayıcı kontrolü (sadece online modda - her 5 saniyede bir)
  static unsigned long lastScheduleCheck = 0;
  if (millis() - lastScheduleCheck > 5000) {
    lastScheduleCheck = millis();
    checkSchedule();
  }

  // Buton kontrolü (debounce ile)
  int buttonReading = digitalRead(BUTTON);
  
  // Buton durumu değiştiyse
  if (buttonReading != currentButtonReading) {
    lastDebounceTime = millis();
    currentButtonReading = buttonReading;
  }
  
  // Debounce süresi geçtiyse
  if ((millis() - lastDebounceTime) > debounceDelay) {
    // Buton durumu gerçekten değiştiyse
    if (currentButtonReading != lastStableButtonState) {
      lastStableButtonState = currentButtonReading;
      
      // Buton basıldığında (HIGH'dan LOW'a geçiş)
      if (lastStableButtonState == LOW) {
        // Röle durumunu değiştir
        relay1State = !relay1State;
        digitalWrite(RELAY1, relay1State ? HIGH : LOW);
        
        Serial.printf("Buton basıldı - Röle: %s\n", relay1State ? "AÇIK" : "KAPALI");
        
        // Blynk'e durumu gönder
        if (Blynk.connected()) {
          Blynk.virtualWrite(V1, relay1State ? 1 : 0);
          
          // Zamanlayıcı aktifse uyarı ver
          if (scheduleEnabled) {
            Serial.println("UYARI: Buton ile manuel müdahale yapıldı, ancak zamanlayıcı hala aktif!");
            Serial.println("Zamanlayıcı bir sonraki kontrolde kurallarını uygulayacak.");
            // lastExecutedMinute'i sıfırla ki hemen kontrol etsin
            lastExecutedMinute = -1;
          }
        }
      }
    }
  }

  // Periyodik bağlantı kontrolü
  if (millis() - lastCheck > checkInterval) {
    lastCheck = millis();
    
    // WiFi kontrolü
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi bağlantısı koptu, yeniden bağlanılıyor...");
      
      int wifiRetryCount = 0;
      while (wifiRetryCount < maxRetries && WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        delay(1000);
        WiFi.reconnect();
        
        unsigned long reconnectStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - reconnectStart < 5000) {
          delay(500);
        }
        
        if (WiFi.status() != WL_CONNECTED) {
          wifiRetryCount++;
          Serial.printf("WiFi yeniden bağlantı denemesi %d/%d başarısız\n", wifiRetryCount, maxRetries);
        }
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi yeniden bağlandı!");
        // Zaman senkronizasyonunu yeniden başlat
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      } else {
        Serial.println("WiFi yeniden bağlanamadı, offline modda devam...");
      }
    }
    
    // Blynk kontrolü
    if (WiFi.status() == WL_CONNECTED && !Blynk.connected()) {
      Serial.println("Blynk bağlantısı koptu, yeniden bağlanılıyor...");
      
      int blynkRetryCount = 0;
      while (blynkRetryCount < maxRetries && !Blynk.connected()) {
        if (Blynk.connect(3000)) {
          Serial.println("Blynk yeniden bağlandı!");
          lastOnlineTime = millis();
          break;
        } else {
          blynkRetryCount++;
          Serial.printf("Blynk yeniden bağlantı denemesi %d/%d başarısız\n", blynkRetryCount, maxRetries);
          if (blynkRetryCount < maxRetries) {
            delay(2000);
          }
        }
      }
    }
    
    // Durum raporu
    Serial.printf("Durum: WiFi=%s, Blynk=%s, Röle=%s, Zamanlayıcı=%s\n",
                  WiFi.status() == WL_CONNECTED ? "OK" : "KOPUK",
                  Blynk.connected() ? "OK" : "KOPUK",
                  relay1State ? "AÇIK" : "KAPALI",
                  scheduleEnabled ? "AKTİF" : "PASİF");
  }

  // Offline modda 1 saatlik otomatik reset kontrolü
  bool isOnline = (WiFi.status() == WL_CONNECTED && Blynk.connected());
  if (!isOnline && (millis() - lastOnlineTime > offlineResetInterval)) {
    Serial.println("Offline modda 1 saat geçti, sistem yeniden başlatılıyor...");
    delay(100);
    ESP.restart();
  }

  // 24 saatlik otomatik reset (online modda)
  if (isOnline && (millis() - bootTime > resetInterval)) {
    Serial.println("24 saat doldu, sistem yeniden başlatılıyor...");
    delay(100);
    ESP.restart();
  }

  // CPU'ya nefes aldır
  delay(10);
}