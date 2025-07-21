#include "WiFi.h"
#include "esp_camera.h"
#include "HTTPClient.h"
//#include "esp_sleep.h"

// Wifi SSID info
const char* WIFI_SSID = "ICT-LAB"; //ICT-LAB 
const char* WIFI_PASSWORD = "Student@sit"; 

// Firebase project Storage Bucket ID 
const char* FIREBASE_STORAGE_BUCKET_ID = "foodpackage-3f995.firebasestorage.app";

#define CAPTURE_INTERVAL 1800 //in seconds
#define MAX_UPLOAD_ATTEMPTS 3

// ===============================================================
// ==> CAMERA PIN & STATE CONFIGURATION
// ===============================================================
bool isCameraOn = false; // Global flag to track camera state

// PIN definitions (for AI-THINKER model)
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

#define LED_BUILTIN       4

// The camera configuration is global so both on() and off() can access it
camera_config_t camera_conf;

// Camera Functions
void cameraOn();
void cameraOff();
int takeAndUploadPhoto();

// Sleep functions / variables
const static uint64_t sleepTime = CAPTURE_INTERVAL * 1000000;

void disableBluetooth(){
    btStop();
}

void connectToWifi() {
  // --- Connect to Hotspot (Simple Method) ---
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected to hotspot!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  Serial.println("==================================");

  //disable bluetooth for power saving
  disableBluetooth();

  //connect to wifi initially
  //connectToWifi();

  //flash pin setup
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // --- Prepare Camera Configuration ---
  camera_conf.ledc_channel = LEDC_CHANNEL_0;
  camera_conf.ledc_timer = LEDC_TIMER_0;
  camera_conf.pin_d0 = Y2_GPIO_NUM; camera_conf.pin_d1 = Y3_GPIO_NUM; camera_conf.pin_d2 = Y4_GPIO_NUM;
  camera_conf.pin_d3 = Y5_GPIO_NUM; camera_conf.pin_d4 = Y6_GPIO_NUM; camera_conf.pin_d5 = Y7_GPIO_NUM;
  camera_conf.pin_d6 = Y8_GPIO_NUM; camera_conf.pin_d7 = Y9_GPIO_NUM;
  camera_conf.pin_xclk = XCLK_GPIO_NUM; camera_conf.pin_pclk = PCLK_GPIO_NUM;
  camera_conf.pin_vsync = VSYNC_GPIO_NUM; camera_conf.pin_href = HREF_GPIO_NUM;
  camera_conf.pin_sccb_sda = SIOD_GPIO_NUM; camera_conf.pin_sccb_scl = SIOC_GPIO_NUM;
  camera_conf.pin_pwdn = PWDN_GPIO_NUM; camera_conf.pin_reset = RESET_GPIO_NUM;
  camera_conf.xclk_freq_hz = 20000000;
  camera_conf.pixel_format = PIXFORMAT_JPEG;
  camera_conf.frame_size = FRAMESIZE_XGA; // High resolution
  camera_conf.jpeg_quality = 20; // Lower number = higher quality
  camera_conf.fb_count = 2; // Use 2 frame buffers for better performance

  //sensor_t * s = esp_camera_sensor_get();
  //s->set_brightness(s, 1);
  delay(8000);
  Serial.println("Starting program...");

}

static int photoCount = 0;
static int cycleCount = 0;

void loop() {
  //
  Serial.print("Starting cycle ");
  Serial.print(cycleCount);
  Serial.print("...\n");

  //connect to wifi
  connectToWifi();
  delay(5000);

  int uploadAttempts = 0;
  
  Serial.print("Turning camera on...\n");
  cameraOn();
  delay(500);
  Serial.print("Taking photo and uploading...\n");
  
  while (uploadAttempts < MAX_UPLOAD_ATTEMPTS) {
   if (takeAndUploadPhoto() == 200) {
    uploadAttempts = 9999;
    Serial.print("Upload successful, continuing program...\n");
   }
   else {
    uploadAttempts++;
    Serial.print("Upload failed on attempt ");
    Serial.print(uploadAttempts);
    Serial.print(". Trying again...\n");
    delay(5000); //wait 5 seconds
   }
  }
  Serial.print("Upload(s) attempted, reset attempt counter and continuing program...\n");
  uploadAttempts = 0;
  /*//Rename up to n number of photos
  if (photoCount > 2) {
    photoCount = 0;
  }
  else {
    photoCount++;
  }
  */
  photoCount++;
  cycleCount++;
  Serial.print("Switching off camera...\n");
  cameraOff();
  /*
  Serial.print("Waiting...\n");
  for (int waitTime = 0; waitTime < CAPTURE_INTERVAL; waitTime++) {
    Serial.print("Waited ");
    Serial.print(waitTime);
    Serial.print("s...\n");
    delay(1000);
  } 
  */
  
  //turn off wifi
  Serial.print("Switching off WiFi...\n");
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);

  //sleep stuff
  Serial.print("Going to sleep for ");
  Serial.print(CAPTURE_INTERVAL);
  Serial.print(" seconds...\n");
  delay(1000);
  esp_sleep_enable_timer_wakeup(sleepTime); //wake up timer in microseconds
  esp_light_sleep_start();
}

void cameraOn() {
  if (isCameraOn) {
    Serial.println("Camera is already on.");
    return;
  }
  Serial.println("Turning camera on...");
  esp_err_t err = esp_camera_init(&camera_conf);
  //sensor_t * s = esp_camera_sensor_get();
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }
   // Configure camera sensor settings optimized for flash photography in dark environments
  sensor_t * s = esp_camera_sensor_get();
  if (s != NULL) {
    // Optimize settings for flash photography to prevent overexposure and green tint
    s->set_brightness(s, -1);    // -2 to 2 (reduced brightness to compensate for flash)
    s->set_contrast(s, 2);       // -2 to 2 (increased contrast for better detail)
    s->set_saturation(s, 1);     // -2 to 2 (increased saturation to counter green tint)
    s->set_special_effect(s, 0); // 0 to 6 (0 - No Effect)
    s->set_whitebal(s, 1);       // 0 = disable , 1 = enable (enable for color correction)
    s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable (enable auto white balance gain)
    s->set_wb_mode(s, 4);        // 0 to 4 - Home mode for indoor flash photography
    s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
    s->set_aec2(s, 1);           // 0 = disable , 1 = enable (enable advanced exposure control)
    s->set_ae_level(s, -2);      // -2 to 2 (reduce exposure level to prevent overexposure)
    s->set_aec_value(s, 150);    // 0 to 1200 (lower exposure value for flash)
    s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30 (keep gain low to prevent noise)
    s->set_gainceiling(s, (gainceiling_t)2);  // 0 to 6 (limit gain ceiling)
    s->set_bpc(s, 1);            // 0 = disable , 1 = enable (bad pixel correction)
    s->set_wpc(s, 1);            // 0 = disable , 1 = enable (white pixel correction)
    s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable (gamma correction)
    s->set_lenc(s, 1);           // 0 = disable , 1 = enable (lens correction)
    s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
    s->set_vflip(s, 0);          // 0 = disable , 1 = enable
    s->set_dcw(s, 1);            // 0 = disable , 1 = enable
    s->set_colorbar(s, 0);       // 0 = disable , 1 = enable

    Serial.println("Camera sensor settings optimized for flash photography in dark environments.");
  } else {
    Serial.println("Warning: Could not get camera sensor for configuration.");
  }

  isCameraOn = true;
  Serial.println("Camera is now ON.");
}

void cameraOff() {
  if (!isCameraOn) {
    Serial.println("Camera is already off.");
    return;
  }
  Serial.println("Turning camera off...");
  esp_err_t err = esp_camera_deinit();
  if (err != ESP_OK) {
    Serial.printf("Camera deinit failed with error 0x%x", err);
    return;
  }
  isCameraOn = false;
  Serial.println("Camera is now OFF. Memory has been freed.");
}

int takeAndUploadPhoto() {
  //TODO: turn on LED for light then turn off
  Serial.print("Switching on flashlight...\n");
  digitalWrite(LED_BUILTIN, HIGH);
  delay(50);

  Serial.println("Taking picture...");
  camera_fb_t * fb = NULL;
  fb = esp_camera_fb_get();
  esp_camera_fb_return(fb); 
  delay(500);
  fb = NULL;
  fb = esp_camera_fb_get();
  esp_camera_fb_return(fb); 
  delay(500);
  fb = NULL;
  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    //return;
  }
  Serial.printf("Picture taken! Size: %zu bytes\n", fb->len);
  Serial.print("Picture ");
  Serial.print(photoCount);
  Serial.print(" taken...\n");

  Serial.print("Switching off flashlight...\n");
  digitalWrite(LED_BUILTIN, LOW); 

  HTTPClient http;
  String imageName = "picture-" + String(photoCount) + ".jpeg";
  String url = "https://firebasestorage.googleapis.com/v0/b/" + String(FIREBASE_STORAGE_BUCKET_ID) + "/o/" + imageName;
  
  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");
  int httpResponseCode = http.POST(fb->buf, fb->len);
  
  esp_camera_fb_return(fb); 
  
  if (httpResponseCode == 200) {
    Serial.printf("Image uploaded successfully! (HTTP %d)\n", httpResponseCode);
  } else {
    Serial.printf("Image upload failed, error: %d - %s\n", httpResponseCode, http.errorToString(httpResponseCode).c_str());
  }
  http.end();
  return httpResponseCode;
}