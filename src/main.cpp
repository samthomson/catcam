#include <Arduino.h>

// Include secrets (WiFi, Nostr keys, Blossom server)
#ifndef SECRETS_FILE
  #error "SECRETS_FILE must be defined! Use -DSECRETS_FILE=\"secrets.h\" in platformio.ini"
#endif
#include SECRETS_FILE

// ESP32-CAM specific includes
#include "esp_camera.h"
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
#include <WiFi.h>
  #include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
  #include <mbedtls/sha256.h>
  #include "uECC.h"

// ===========================================
// ESP32-CAM AI-Thinker Pin Definitions
// ===========================================
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

// Built-in LED - GPIO4 is the FLASH LED (very bright!)
// Set to -1 to disable, or 4 to use flash for status
#define LED_GPIO_NUM      -1  // Disabled - flash is too bright for status

// ===========================================
// Nostr/Schnorr Signature Constants
// ===========================================

// secp256k1 curve order n
static const uint8_t SECP256K1_ORDER[32] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE,
  0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48, 0xA0, 0x3B,
  0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41
};

// Pre-computed tag hashes for BIP-340
static const uint8_t TAG_CHALLENGE[32] = {
  0x7b, 0xb5, 0x2d, 0x7a, 0x9f, 0xef, 0x58, 0x32,
  0x3e, 0xb1, 0xbf, 0x7a, 0x40, 0x7d, 0xb3, 0x82,
  0xd2, 0xf3, 0xf2, 0xd8, 0x1b, 0xb1, 0x22, 0x4f,
  0x49, 0xfe, 0x51, 0x8f, 0x6d, 0x48, 0xd3, 0x7c
};
static const uint8_t TAG_NONCE[32] = {
  0x07, 0x49, 0x77, 0x34, 0xa7, 0x9b, 0xcb, 0x35,
  0x5b, 0x9b, 0x8c, 0x7d, 0x03, 0x4f, 0x12, 0x1c,
  0xf4, 0x34, 0xd7, 0x3a, 0xf6, 0x39, 0x7b, 0x96,
  0xb8, 0x0a, 0xab, 0x80, 0x51, 0x85, 0x45, 0x69
};
static const uint8_t TAG_AUX[32] = {
  0xf1, 0xef, 0x4e, 0x5e, 0xc0, 0x63, 0xca, 0xda,
  0xf7, 0xef, 0xf4, 0xb9, 0x8e, 0x3b, 0x67, 0xc8,
  0xa7, 0x90, 0xae, 0xfe, 0x0f, 0x05, 0xfc, 0x7a,
  0x5f, 0x21, 0xa9, 0x93, 0x01, 0x6c, 0x3a, 0xba
};

// ===========================================
// Global Variables
// ===========================================
char ssid[] = WIFI_SSID;
char pass[] = WIFI_PASS;
const char* nostrPrivateKeyHex = NOSTR_PRIVKEY;
const char* blossomServer = BLOSSOM_SERVER;
const char* nostrRelayHost = NOSTR_RELAY_HOST;
const char* cameraName = CAMERA_NAME;
const unsigned long CAPTURE_INTERVAL = 30000;  // 30 seconds

String nostrPubkey = "";
uint8_t privKeyBytes[32];
uint8_t pubKeyBytes[32];

// ===========================================
// Forward Declarations
// ===========================================
void connectWiFi();
bool initCamera();
camera_fb_t* capturePhoto();
String uploadToBlossom(camera_fb_t* fb);
bool publishNostrEvent(const String& imageUrl, int rssi);
void blinkLED(int times, int delayMs);

// Crypto functions
void derivePubkey();
String bytesToHex(uint8_t* bytes, int len);
void hexToBytes(const char* hex, uint8_t* bytes, int len);
void sha256Raw(const uint8_t* data, size_t len, uint8_t* out);
void taggedHash(const uint8_t* tag, const uint8_t* data, size_t len, uint8_t* out);
bool schnorrSign(const uint8_t* privkey, const uint8_t* msg, uint8_t* sig);
String createAuthEvent(const String& url, const String& method, const String& sha256Hash);

// ===========================================
// Big Number Operations (for Schnorr)
// ===========================================
int bn_compare(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < 32; i++) {
    if (a[i] > b[i]) return 1;
    if (a[i] < b[i]) return -1;
  }
  return 0;
}

void bn_sub(const uint8_t* a, const uint8_t* b, uint8_t* result) {
  int16_t borrow = 0;
  for (int i = 31; i >= 0; i--) {
    int16_t diff = (int16_t)a[i] - (int16_t)b[i] - borrow;
    if (diff < 0) {
      diff += 256;
      borrow = 1;
    } else {
      borrow = 0;
    }
    result[i] = (uint8_t)diff;
  }
}

void bn_add(const uint8_t* a, const uint8_t* b, uint8_t* result) {
  uint16_t carry = 0;
  for (int i = 31; i >= 0; i--) {
    uint16_t sum = (uint16_t)a[i] + (uint16_t)b[i] + carry;
    result[i] = (uint8_t)(sum & 0xFF);
    carry = sum >> 8;
  }
}

void bn_mod(uint8_t* a, const uint8_t* n) {
  while (bn_compare(a, n) >= 0) {
    bn_sub(a, n, a);
  }
}

void bn_add_mod(const uint8_t* a, const uint8_t* b, const uint8_t* n, uint8_t* result) {
  uint8_t temp[33] = {0};
  uint16_t carry = 0;
  for (int i = 31; i >= 0; i--) {
    uint16_t sum = (uint16_t)a[i] + (uint16_t)b[i] + carry;
    temp[i + 1] = (uint8_t)(sum & 0xFF);
    carry = sum >> 8;
  }
  temp[0] = carry;
  
  uint8_t* ptr = temp + 1;
  if (temp[0] || bn_compare(ptr, n) >= 0) {
    bn_sub(ptr, n, ptr);
  }
  memcpy(result, ptr, 32);
}

void bn_mul_mod(const uint8_t* a, const uint8_t* b, const uint8_t* n, uint8_t* result) {
  uint8_t acc[32] = {0};
  uint8_t temp[32];
  memcpy(temp, a, 32);
  
  for (int i = 31; i >= 0; i--) {
    for (int bit = 0; bit < 8; bit++) {
      if ((b[i] >> bit) & 1) {
        bn_add_mod(acc, temp, n, acc);
      }
      bn_add_mod(temp, temp, n, temp);
    }
  }
  
  memcpy(result, acc, 32);
}

static int RNG(uint8_t *dest, unsigned size) {
  while (size--) *dest++ = (uint8_t)random(256);
  return 1;
}

// ===========================================
// Setup
// ===========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Disable brownout detector (ESP32-CAM is sensitive)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  // Initialize LED (if enabled)
  if (LED_GPIO_NUM >= 0) {
    pinMode(LED_GPIO_NUM, OUTPUT);
    digitalWrite(LED_GPIO_NUM, HIGH);  // LED off (active LOW)
  }
  
  Serial.println("\n===========================================");
  Serial.println("       CAT CAM - ESP32-CAM");
  Serial.println("===========================================");
  Serial.printf("Camera: %s\n", cameraName);
  Serial.println("Capture interval: 30 seconds");
  
  // Initialize random seed and crypto
  randomSeed(analogRead(0) ^ micros());
  uECC_set_rng(&RNG);
  
  // Initialize camera
  if (!initCamera()) {
    Serial.println("Camera init failed! Restarting...");
    delay(3000);
    ESP.restart();
  }
  Serial.println("Camera initialized!");
  blinkLED(2, 100);
  
  // Connect to WiFi
  connectWiFi();
  blinkLED(3, 100);
  
  // Sync time (needed for Nostr events)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing time");
  time_t now = time(nullptr);
  while (now < 1700000000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println(" OK");
  
  // Derive Nostr public key
  derivePubkey();
  
  Serial.println("\n===========================================");
  Serial.println("Ready! Taking photos every 30 seconds");
  Serial.println("===========================================\n");
}

// ===========================================
// Main Loop
// ===========================================
void loop() {
  static unsigned long lastCapture = 0;
  unsigned long now = millis();
  
  // Check if it's time to capture
  if (now - lastCapture >= CAPTURE_INTERVAL || lastCapture == 0) {
    lastCapture = now;
    
    Serial.println("\n--- Capturing photo ---");
    blinkLED(1, 50);
    
    // Capture photo
    camera_fb_t* fb = capturePhoto();
    if (fb) {
      Serial.printf("Photo captured! Size: %u bytes\n", fb->len);
      
      // Check WiFi connection
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected! Reconnecting...");
        WiFi.reconnect();
        delay(5000);
      }
      
      int rssi = WiFi.RSSI();
      Serial.printf("WiFi signal: %d dBm\n", rssi);
      
      // Upload to Blossom
      String blobUrl = uploadToBlossom(fb);
      if (blobUrl.length() > 0) {
        Serial.println("Upload successful!");
        Serial.println("URL: " + blobUrl);
        blinkLED(2, 100);
        
        // Post to Nostr relay with signal strength
        if (publishNostrEvent(blobUrl, rssi)) {
          Serial.println("Posted to Nostr!");
        } else {
          Serial.println("Nostr post failed");
        }
      } else {
        Serial.println("Upload failed!");
        Serial.printf("WiFi still connected: %s\n", WiFi.status() == WL_CONNECTED ? "yes" : "no");
        blinkLED(5, 50);
      }
      
      // Return frame buffer
      esp_camera_fb_return(fb);
    } else {
      Serial.println("Capture failed!");
      blinkLED(5, 50);
    }
  }
  
  delay(100);
}

// ===========================================
// WiFi Connection
// ===========================================
void connectWiFi() {
  Serial.printf("SSID: '%s'\n", ssid);
  Serial.printf("Password length: %d\n", strlen(pass));
  
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // Max WiFi power
  WiFi.setSleep(false);  // Disable WiFi sleep for better reliability
  WiFi.begin(ssid, pass);
  Serial.print("Connecting to WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {  // Increased to 60 attempts (30 sec)
    delay(500);
    Serial.print(".");
    attempts++;
    
    // Print status every 10 attempts
    if (attempts % 10 == 0) {
      Serial.printf(" [status: %d]", WiFi.status());
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Signal strength: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println(" FAILED");
    Serial.printf("WiFi status code: %d\n", WiFi.status());
    Serial.println("Status codes: 0=IDLE, 1=NO_SSID_AVAIL, 4=CONNECT_FAILED, 6=DISCONNECTED");
    Serial.println("Restarting in 5 seconds...");
    delay(5000);
    ESP.restart();
  }
}

// ===========================================
// Camera Initialization
// ===========================================
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
  config.grab_mode = CAMERA_GRAB_LATEST;
  
  // Use PSRAM for higher resolution if available
  if (psramFound()) {
    Serial.println("PSRAM found - using high resolution");
    config.frame_size = FRAMESIZE_UXGA;  // 1600x1200
    config.jpeg_quality = 10;  // 0-63, lower = better quality
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    Serial.println("No PSRAM - using lower resolution");
    config.frame_size = FRAMESIZE_SVGA;  // 800x600
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }
  
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }
  
  // Adjust camera settings for better quality
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);     // -2 to 2
    s->set_whitebal(s, 1);       // 0 = disable, 1 = enable
    s->set_awb_gain(s, 1);       // 0 = disable, 1 = enable
    s->set_wb_mode(s, 0);        // 0 to 4 - white balance mode
    s->set_exposure_ctrl(s, 1);  // 0 = disable, 1 = enable
    s->set_aec2(s, 1);           // 0 = disable, 1 = enable
    s->set_gain_ctrl(s, 1);      // 0 = disable, 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30
    s->set_gainceiling(s, (gainceiling_t)6);  // 0 to 6
    s->set_bpc(s, 1);            // 0 = disable, 1 = enable
    s->set_wpc(s, 1);            // 0 = disable, 1 = enable
    s->set_raw_gma(s, 1);        // 0 = disable, 1 = enable
    s->set_lenc(s, 1);           // 0 = disable, 1 = enable
    s->set_hmirror(s, 1);        // 1 = mirror horizontally (fix reversed image)
    s->set_vflip(s, 0);          // 0 = no vertical flip
  }
  
  return true;
}

// ===========================================
// Photo Capture
// ===========================================
camera_fb_t* capturePhoto() {
  // Take a few frames to let auto-exposure settle
  for (int i = 0; i < 3; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
    delay(100);
  }
  
  // Capture the actual photo
  return esp_camera_fb_get();
}

// ===========================================
// Blossom Upload (with NIP-98 auth)
// ===========================================
String uploadToBlossom(camera_fb_t* fb) {
  if (!fb || fb->len == 0) {
    Serial.println("No image data to upload");
    return "";
  }
  
  // Calculate SHA-256 hash of the image
  uint8_t hash[32];
  sha256Raw(fb->buf, fb->len, hash);
  String sha256Hash = bytesToHex(hash, 32);
  Serial.println("Image SHA-256: " + sha256Hash);
  
  // Build upload URL
  String uploadUrl = String(blossomServer) + "/upload";
  
  // Create NIP-98 authorization event
  String authEvent = createAuthEvent(uploadUrl, "PUT", sha256Hash);
  if (authEvent.length() == 0) {
    Serial.println("Failed to create auth event");
    return "";
  }
  
  // Base64 encode the auth event for the header
  // Simple base64 encoding
  const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String base64Auth = "";
  const uint8_t* input = (const uint8_t*)authEvent.c_str();
  size_t inputLen = authEvent.length();
  
  for (size_t i = 0; i < inputLen; i += 3) {
    uint32_t octet_a = i < inputLen ? input[i] : 0;
    uint32_t octet_b = i + 1 < inputLen ? input[i + 1] : 0;
    uint32_t octet_c = i + 2 < inputLen ? input[i + 2] : 0;
    uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;
    
    base64Auth += base64_chars[(triple >> 18) & 0x3F];
    base64Auth += base64_chars[(triple >> 12) & 0x3F];
    base64Auth += (i + 1 < inputLen) ? base64_chars[(triple >> 6) & 0x3F] : '=';
    base64Auth += (i + 2 < inputLen) ? base64_chars[triple & 0x3F] : '=';
  }
  
  Serial.println("Uploading to: " + uploadUrl);
  Serial.printf("Image size: %u bytes\n", fb->len);
  
  // Create HTTPS client
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(60);  // 60 sec SSL timeout
  
  HTTPClient http;
  if (!http.begin(client, uploadUrl)) {
    Serial.println("Failed to begin HTTP connection");
    return "";
  }
  
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("Authorization", "Nostr " + base64Auth);
  http.setTimeout(60000);  // 60 second timeout
  
  // Upload the image
  Serial.println("Starting upload...");
  int httpCode = http.PUT(fb->buf, fb->len);
  
  String result = "";
  if (httpCode > 0) {
    Serial.printf("HTTP response code: %d\n", httpCode);
    String response = http.getString();
    Serial.println("Response: " + response);
    
    if (httpCode == 200 || httpCode == 201) {
      // Parse response to get URL
      StaticJsonDocument<512> doc;
      DeserializationError error = deserializeJson(doc, response);
      if (!error) {
        if (doc.containsKey("url")) {
          result = doc["url"].as<String>();
        } else if (doc.containsKey("sha256")) {
          // Construct URL from hash
          result = String(blossomServer) + "/" + doc["sha256"].as<String>();
        }
      }
    }
  } else {
    Serial.printf("HTTP error: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
  return result;
}

// ===========================================
// NIP-98 Authorization Event
// ===========================================
String createAuthEvent(const String& url, const String& method, const String& sha256Hash) {
  unsigned long createdAt = (unsigned long)time(nullptr);
  unsigned long expiration = createdAt + 300;  // 5 minutes from now
  
  // Build tags array for Blossom BUD-02 auth
  String tags = "[";
  tags += "[\"t\",\"upload\"],";  // Action type
  tags += "[\"x\",\"" + sha256Hash + "\"],";  // File hash
  tags += "[\"expiration\",\"" + String(expiration) + "\"]";  // Required!
  tags += "]";
  
  // Canonical form for event ID (kind 24242 = Blossom auth)
  int authKind = 24242;
  String canonical = "[0,\"" + nostrPubkey + "\"," + String(createdAt) + "," + String(authKind) + "," + tags + ",\"\"]";
  
  // Calculate event ID
  uint8_t eventIdBytes[32];
  sha256Raw((const uint8_t*)canonical.c_str(), canonical.length(), eventIdBytes);
  String eventId = bytesToHex(eventIdBytes, 32);
  
  // Sign the event
  uint8_t sig[64];
  if (!schnorrSign(privKeyBytes, eventIdBytes, sig)) {
    Serial.println("Failed to sign auth event");
    return "";
  }
  String signature = bytesToHex(sig, 64);
  
  // Build the event JSON
  String event = "{\"id\":\"" + eventId + "\",";
  event += "\"pubkey\":\"" + nostrPubkey + "\",";
  event += "\"created_at\":" + String(createdAt) + ",";
  event += "\"kind\":" + String(authKind) + ",";
  event += "\"tags\":" + tags + ",";
  event += "\"content\":\"\",";
  event += "\"sig\":\"" + signature + "\"}";
  
  return event;
}

// ===========================================
// Publish Nostr Event (kind 1 note)
// ===========================================
bool publishNostrEvent(const String& imageUrl, int rssi) {
  unsigned long createdAt = (unsigned long)time(nullptr);
  
  // Build content with image URL
  String content = imageUrl;
  
  // Build tags array with imeta and signal strength
  String tags = "[[\"imeta\",\"url " + imageUrl + "\",\"m image/jpeg\"]";
  tags += ",[\"signal\",\"" + String(rssi) + " dBm\"]]";
  
  // Canonical form for event ID (kind 1 = text note)
  int kind = 1;
  String canonical = "[0,\"" + nostrPubkey + "\"," + String(createdAt) + "," + String(kind) + "," + tags + ",\"" + content + "\"]";
  
  // Calculate event ID
  uint8_t eventIdBytes[32];
  sha256Raw((const uint8_t*)canonical.c_str(), canonical.length(), eventIdBytes);
  String eventId = bytesToHex(eventIdBytes, 32);
  
  // Sign the event
  uint8_t sig[64];
  if (!schnorrSign(privKeyBytes, eventIdBytes, sig)) {
    Serial.println("Failed to sign Nostr event");
    return false;
  }
  String signature = bytesToHex(sig, 64);
  
  // Build the event JSON
  String event = "{\"id\":\"" + eventId + "\",";
  event += "\"pubkey\":\"" + nostrPubkey + "\",";
  event += "\"created_at\":" + String(createdAt) + ",";
  event += "\"kind\":" + String(kind) + ",";
  event += "\"tags\":" + tags + ",";
  event += "\"content\":\"" + content + "\",";
  event += "\"sig\":\"" + signature + "\"}";
  
  // Send to relay
  Serial.println("Publishing to relay: " + String(nostrRelayHost));
  
  WiFiClientSecure client;
  client.setInsecure();
  
  // Connect to WebSocket relay
  if (!client.connect(nostrRelayHost, 443)) {
    Serial.println("Failed to connect to relay");
    return false;
  }
  
  // Send WebSocket upgrade request
  String wsRequest = "GET / HTTP/1.1\r\n";
  wsRequest += "Host: " + String(nostrRelayHost) + "\r\n";
  wsRequest += "Upgrade: websocket\r\n";
  wsRequest += "Connection: Upgrade\r\n";
  wsRequest += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
  wsRequest += "Sec-WebSocket-Version: 13\r\n\r\n";
  
  client.print(wsRequest);
  
  // Wait for upgrade response
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println("Relay connection timeout");
      client.stop();
      return false;
    }
    delay(10);
  }
  
  // Read upgrade response
  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  
  // Send EVENT message (WebSocket text frame)
  String eventMsg = "[\"EVENT\"," + event + "]";
  
  // WebSocket frame: FIN=1, opcode=1 (text), mask=1, payload
  uint8_t header[10];
  int headerLen = 2;
  header[0] = 0x81;  // FIN + text frame
  
  size_t payloadLen = eventMsg.length();
  if (payloadLen < 126) {
    header[1] = 0x80 | payloadLen;  // Mask bit + length
  } else {
    header[1] = 0x80 | 126;
    header[2] = (payloadLen >> 8) & 0xFF;
    header[3] = payloadLen & 0xFF;
    headerLen = 4;
  }
  
  // Masking key (random)
  uint8_t mask[4] = {
    (uint8_t)random(256),
    (uint8_t)random(256),
    (uint8_t)random(256),
    (uint8_t)random(256)
  };
  memcpy(header + headerLen, mask, 4);
  headerLen += 4;
  
  // Send header
  client.write(header, headerLen);
  
  // Send masked payload
  for (size_t i = 0; i < payloadLen; i++) {
    client.write(eventMsg[i] ^ mask[i % 4]);
  }
  
  // Wait for OK response
  timeout = millis();
  bool gotOk = false;
  while (millis() - timeout < 3000) {
    if (client.available()) {
      // Read WebSocket frame
      uint8_t frameByte = client.read();
      if ((frameByte & 0x0F) == 0x01) {  // Text frame
        uint8_t lenByte = client.read();
        size_t len = lenByte & 0x7F;
        if (len == 126) {
          len = (client.read() << 8) | client.read();
        }
        
        String response = "";
        for (size_t i = 0; i < len && i < 500; i++) {
          if (client.available()) {
            response += (char)client.read();
          }
        }
        
        Serial.println("Relay response: " + response);
        if (response.indexOf("\"OK\"") >= 0 && response.indexOf("true") >= 0) {
          gotOk = true;
          break;
        }
      }
    }
    delay(10);
  }
  
  client.stop();
  return gotOk;
}

// ===========================================
// Crypto Functions
// ===========================================
void derivePubkey() {
  hexToBytes(nostrPrivateKeyHex, privKeyBytes, 32);
  
  uint8_t pubFull[64];
  uECC_Curve curve = uECC_secp256k1();
  uECC_compute_public_key(privKeyBytes, pubFull, curve);
  
  // x-coordinate only (first 32 bytes)
  memcpy(pubKeyBytes, pubFull, 32);
  nostrPubkey = bytesToHex(pubKeyBytes, 32);
  Serial.print("Nostr Pubkey: ");
  Serial.println(nostrPubkey);
}

String bytesToHex(uint8_t* bytes, int len) {
  String hex = "";
  for (int i = 0; i < len; i++) {
    if (bytes[i] < 16) hex += "0";
    hex += String(bytes[i], HEX);
  }
  return hex;
}

void hexToBytes(const char* hex, uint8_t* bytes, int len) {
  for (int i = 0; i < len; i++) {
    char buf[3] = {hex[i*2], hex[i*2+1], 0};
    bytes[i] = strtol(buf, NULL, 16);
  }
}

void sha256Raw(const uint8_t* data, size_t len, uint8_t* out) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

void taggedHash(const uint8_t* tag, const uint8_t* data, size_t len, uint8_t* out) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, tag, 32);
    mbedtls_sha256_update(&ctx, tag, 32);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

// BIP-340 Schnorr signature
bool schnorrSign(const uint8_t* privkey, const uint8_t* msg32, uint8_t* sig64) {
  uECC_Curve curve = uECC_secp256k1();
  
  // 1. Compute public key P = d*G
  uint8_t pubFull[64];
  if (!uECC_compute_public_key(privkey, pubFull, curve)) return false;
  uint8_t px[32];
  memcpy(px, pubFull, 32);
  
  // 2. Check if P.y is even, negate privkey if odd
  uint8_t d[32];
  memcpy(d, privkey, 32);
  if (pubFull[63] & 1) {
    bn_sub(SECP256K1_ORDER, d, d);
  }
  
  // 3. Generate deterministic nonce k
  uint8_t aux[32];
  for (int i = 0; i < 32; i++) aux[i] = random(256);
  
  uint8_t t[32], auxHash[32];
  taggedHash(TAG_AUX, aux, 32, auxHash);
  for (int i = 0; i < 32; i++) t[i] = d[i] ^ auxHash[i];
  
  uint8_t nonceData[96];
  memcpy(nonceData, t, 32);
  memcpy(nonceData + 32, px, 32);
  memcpy(nonceData + 64, msg32, 32);
  uint8_t kPrime[32];
  taggedHash(TAG_NONCE, nonceData, 96, kPrime);
  
  bn_mod(kPrime, SECP256K1_ORDER);
  bool isZero = true;
  for (int i = 0; i < 32; i++) if (kPrime[i]) isZero = false;
  if (isZero) return false;
  
  // 4. R = k' * G
  uint8_t R[64];
  if (!uECC_compute_public_key(kPrime, R, curve)) return false;
  uint8_t rx[32];
  memcpy(rx, R, 32);
  
  // 5. Check if R.y is even, negate k if odd
  uint8_t k[32];
  memcpy(k, kPrime, 32);
  if (R[63] & 1) {
    bn_sub(SECP256K1_ORDER, k, k);
  }
  
  // 6. e = tagged_hash("BIP0340/challenge", rx || px || msg) mod n
  uint8_t challengeData[96];
  memcpy(challengeData, rx, 32);
  memcpy(challengeData + 32, px, 32);
  memcpy(challengeData + 64, msg32, 32);
  uint8_t e[32];
  taggedHash(TAG_CHALLENGE, challengeData, 96, e);
  bn_mod(e, SECP256K1_ORDER);
  
  // 7. s = (k + e * d) mod n
  uint8_t ed[32];
  bn_mul_mod(e, d, SECP256K1_ORDER, ed);
  uint8_t s[32];
  bn_add_mod(k, ed, SECP256K1_ORDER, s);
  
  // 8. Signature = rx || s
  memcpy(sig64, rx, 32);
  memcpy(sig64 + 32, s, 32);
  
  return true;
}

// ===========================================
// LED Helper
// ===========================================
void blinkLED(int times, int delayMs) {
  if (LED_GPIO_NUM < 0) return;  // LED disabled
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_GPIO_NUM, LOW);   // LED on
    delay(delayMs);
    digitalWrite(LED_GPIO_NUM, HIGH);  // LED off
    delay(delayMs);
  }
}
