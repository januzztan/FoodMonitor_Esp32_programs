#define ENABLE_FIRESTORE

#include <FirebaseClient.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include <time.h>
#include "ExampleFunctions.h"

// WiFi and Firebase Configuration
#define WIFI_SSID "Casa Monts & Bails"
#define WIFI_PASSWORD "monty@140AB"
#define FIREBASE_PROJECT_ID "foodpackage-3f995"

// Modbus Configuration
#define MODBUS_SERIAL Serial2
#define MODBUS_SLAVE_ID 1

// Settings
#define DEBUG_ENABLED false  // Enable debug output
#define SENSOR_READ_INTERVAL 10000   // Read sensors every 10 seconds
#define FIREBASE_SEND_INTERVAL 60000 // Send to Firebase every 1 minute
#define WIFI_RECONNECT_INTERVAL 5000 // Try WiFi reconnect every 5 seconds
#define MODBUS_RETRY_INTERVAL 5000   // Retry Modbus init every 5 seconds
#define MAX_UPLOAD_RETRIES 5         // Maximum number of upload retry attempts
#define UPLOAD_RETRY_DELAY 3000      // Delay between upload retries (3 seconds)

// Function declarations
void processFirebaseData(AsyncResult &aResult);
void readModbusSensors();
void sendSensorDataToFirebase();
String getCurrentTimestamp();
String getCurrentDateTime();
String generateRandomDocumentId();
bool validateConfiguration();
bool initializeModbus();
bool initializeWiFi();
void setupNTP();
void debugPrint(String message);

// Firebase objects
SSL_CLIENT ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

NoAuth no_auth;
FirebaseApp app;
Firestore::Documents Docs;
AsyncResult firestoreResult;

// Modbus object
ModbusMaster node;

// Sensor data structure
struct SensorData {
    float temperature;
    float humidity;
    float pressure;
    bool isValid;
    unsigned long timestamp;
    int errorCode;
};

// State variables
SensorData currentSensorData = {0.0, 0.0, 0.0, false, 0, 0};
unsigned long lastSensorRead = 0;
unsigned long lastFirebaseSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastModbusRetry = 0;
bool configValid = false;
bool modbusInitialized = false;
bool wifiInitialized = false;
bool ntpInitialized = false;
bool waitingForUpload = false;
bool firebaseInitialized = false;
int uploadRetryCount = 0;
unsigned long lastUploadRetry = 0;
bool firstUploadDone = false;

void setup()
{
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=== Modbus Sensor to Firebase (Random IDs) ===");
    
    // Initialize random seed
    randomSeed(analogRead(0) + millis());
    
    // Validate configuration
    configValid = validateConfiguration();
    if (!configValid) {
        Serial.println("Configuration validation failed!");
        return;
    }
    
    // Initialize components
    wifiInitialized = initializeWiFi();
    if (wifiInitialized) {
        setupNTP();
        modbusInitialized = initializeModbus();
        
        if (modbusInitialized) {
            // Initialize Firebase
            Serial.println("Initializing Firebase...");
            set_ssl_client_insecure_and_buffer(ssl_client);
            initializeApp(aClient, app, getAuth(no_auth));
            app.getApp<Firestore::Documents>(Docs);
            
            firebaseInitialized = true;
            Serial.println("Firebase initialized");
            Serial.println("Starting sensor monitoring...");
            Serial.println("=== Readings every 10s | Upload every 1min ===\n");
            
            // Initialize timing variables properly
            lastSensorRead = millis();
            lastFirebaseSend = millis();
        }
    }
}

void loop()
{
    // Check if we need to recover from failed initialization
    if (!configValid) {
        delay(5000);
        configValid = validateConfiguration();
        return;
    }
    
    // Check WiFi connection and attempt reconnection if needed
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWiFiCheck >= WIFI_RECONNECT_INTERVAL) {
            debugPrint("WiFi disconnected, attempting reconnect...");
            wifiInitialized = initializeWiFi();
            lastWiFiCheck = millis();
        }
        delay(1000);
        return;
    }
    
    // Initialize NTP if not done yet
    if (!ntpInitialized && wifiInitialized) {
        setupNTP();
    }
    
    // Try to initialize Modbus if not done yet
    if (!modbusInitialized) {
        if (millis() - lastModbusRetry >= MODBUS_RETRY_INTERVAL) {
            debugPrint("Retrying Modbus initialization...");
            modbusInitialized = initializeModbus();
            lastModbusRetry = millis();
        }
        delay(1000);
        return;
    }
    
    // Maintain Firebase app
    if (firebaseInitialized) {
        app.loop();
        
        // Debug: Check Firebase app status
        if (DEBUG_ENABLED && millis() % 30000 < 100) { // Print every 30 seconds
            debugPrint("Firebase app ready: " + String(app.ready() ? "YES" : "NO"));
        }
    }
    
    // Read sensors every 10 seconds
    if (millis() - lastSensorRead >= SENSOR_READ_INTERVAL) {
        readModbusSensors();
        lastSensorRead = millis();
    }
    
    // Debug: Print current state every 15 seconds
    if (DEBUG_ENABLED && millis() % 15000 < 100) {
        debugPrint("=== Current State ===");
        debugPrint("Firebase ready: " + String(app.ready() ? "YES" : "NO"));
        debugPrint("Sensor data valid: " + String(currentSensorData.isValid ? "YES" : "NO"));
        debugPrint("Waiting for upload: " + String(waitingForUpload ? "YES" : "NO"));
        debugPrint("Time since last Firebase send: " + String(millis() - lastFirebaseSend) + "ms");
        debugPrint("==================");
    }
    
    // Send data to Firebase every 1 minute (only if we have valid sensor data)
    // For the first upload, skip the timer check to upload immediately
    bool timeConditionMet = firstUploadDone ? (millis() - lastFirebaseSend >= FIREBASE_SEND_INTERVAL) : true;
    
    if (firebaseInitialized && app.ready() && currentSensorData.isValid && 
        !waitingForUpload && timeConditionMet) {
        
        debugPrint("All conditions met - attempting Firebase upload...");
        uploadRetryCount = 0; // Reset retry count for new upload
        
        if (!firstUploadDone) {
            Serial.println("Starting first upload (no delay)");
        }
        
        sendSensorDataToFirebase();
        lastFirebaseSend = millis();
    }
    
    // Handle upload retries if previous upload failed
    if (uploadRetryCount > 0 && uploadRetryCount <= MAX_UPLOAD_RETRIES && 
        !waitingForUpload && (millis() - lastUploadRetry >= UPLOAD_RETRY_DELAY)) {
        
        Serial.printf("Retrying upload (attempt %d/%d)...\n", uploadRetryCount + 1, MAX_UPLOAD_RETRIES + 1);
        sendSensorDataToFirebase();
    }
    
    // Process Firebase responses
    processFirebaseData(firestoreResult);
    
    delay(100);

    //switch off wifi and bluetooth to save power?
    Serial.print("Switching off WiFi and Bluetooth...\n");
    btStop();
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
}

void debugPrint(String message) {
    if (DEBUG_ENABLED) {
        Serial.println("[DEBUG] " + message);
    }
}

bool initializeWiFi()
{
    Serial.println("Connecting to WiFi...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
        Serial.print(".");
        delay(300);
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi connection failed!");
        return false;
    }
    
    Serial.println("\nWiFi connected");
    Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

bool initializeModbus()
{
    Serial.println("Initializing Modbus...");
    MODBUS_SERIAL.begin(9600, SERIAL_8N1, 16, 17); // RX=16, TX=17
    node.begin(MODBUS_SLAVE_ID, MODBUS_SERIAL);
    
    // Test Modbus connection
    delay(1000);
    uint8_t result = node.readHoldingRegisters(0x0000, 4);
    
    if (result == node.ku8MBSuccess) {
        Serial.println("Modbus initialized and tested successfully");
        return true;
    } else {
        Serial.printf("Modbus initialization test failed. Error: 0x%02X\n", result);
        return false;
    }
}

void setupNTP()
{
    Serial.println("Setting up NTP...");
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // UTC+8
    
    // Wait for time to be set
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        Serial.println("NTP time synchronized");
        ntpInitialized = true;
    } else {
        Serial.println("NTP sync failed, using system time");
        ntpInitialized = false;
    }
}

void readModbusSensors()
{
    debugPrint("Reading Modbus sensors...");
    
    uint8_t result = node.readHoldingRegisters(0x0000, 4);
    
    if (result == node.ku8MBSuccess) {
        // Read and scale sensor values according to datasheet
        int16_t rawTemp = node.getResponseBuffer(0x0000);
        int16_t rawHum = node.getResponseBuffer(0x0001);
        int16_t rawPress = node.getResponseBuffer(0x0003);
        
        // Apply scaling factors
        currentSensorData.temperature = rawTemp / 100.0;
        currentSensorData.humidity = rawHum / 100.0;
        currentSensorData.pressure = rawPress / 10.0;
        currentSensorData.isValid = true;
        currentSensorData.timestamp = millis();
        currentSensorData.errorCode = 0;
        
        // Simple, standardized sensor output
        Serial.println("--- Sensor Reading ---");
        Serial.printf("Temperature: %.1f°C\n", currentSensorData.temperature);
        Serial.printf("Humidity: %.1f%%\n", currentSensorData.humidity);
        Serial.printf("Pressure: %.1f hPa\n", currentSensorData.pressure);
        Serial.println("Status: OK");
        Serial.println("---------------------");
        Serial.println();
        
    } else {
        currentSensorData.isValid = false;
        currentSensorData.errorCode = result;
        
        // Simple, standardized error output
        Serial.println("--- Sensor Reading ---");
        Serial.println("Temperature: ERROR");
        Serial.println("Humidity: ERROR");
        Serial.println("Pressure: ERROR");
        Serial.printf("Status: FAILED (0x%02X)\n", result);
        Serial.println("---------------------");
        Serial.println();
    }
}

String generateRandomDocumentId()
{
    // Generate a random alphanumeric string
    String chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    String randomId = "";
    
    // Create a 16-character random string
    for (int i = 0; i < 16; i++) {
        randomId += chars[random(chars.length())];
    }
    
    // Add timestamp for additional uniqueness
    randomId += "_" + String(millis());
    
    return randomId;
}

void sendSensorDataToFirebase()
{
    Serial.println("=== FIREBASE UPLOAD ATTEMPT ===");
    Serial.printf("Temperature: %.1f°C\n", currentSensorData.temperature);
    Serial.printf("Humidity: %.1f%%\n", currentSensorData.humidity);
    Serial.printf("Pressure: %.1f hPa\n", currentSensorData.pressure);
    Serial.printf("Firebase ready: %s\n", app.ready() ? "YES" : "NO");
    
    if (uploadRetryCount > 0) {
        Serial.printf("Retry attempt: %d/%d\n", uploadRetryCount + 1, MAX_UPLOAD_RETRIES + 1);
    }
    
    waitingForUpload = true;
    
    // Generate random document ID
    String randomDocId = generateRandomDocumentId();
    String documentPath = "sensor_data/" + randomDocId;
    
    Serial.printf("Document ID: %s\n", randomDocId.c_str());
    Serial.printf("Document path: %s\n", documentPath.c_str());
    
    // Create document with sensor data
    Document<Values::Value> doc("timestamp", Values::Value(Values::StringValue(getCurrentTimestamp())));
    doc.add("datetime", Values::Value(Values::StringValue(getCurrentDateTime())));
    doc.add("temperature", Values::Value(Values::DoubleValue(currentSensorData.temperature)));
    doc.add("humidity", Values::Value(Values::DoubleValue(currentSensorData.humidity)));
    doc.add("pressure", Values::Value(Values::DoubleValue(currentSensorData.pressure)));
    
    Serial.println("Sending to Firestore...");
    Docs.createDocument(aClient, Firestore::Parent(FIREBASE_PROJECT_ID), documentPath, DocumentMask(), doc, firestoreResult);
    Serial.println("===============================");
}

void processFirebaseData(AsyncResult &aResult)
{
    if (!aResult.isResult()) return;
    
    if (aResult.isError()) {
        // Handle retry logic
        uploadRetryCount++;
        
        Serial.println("--- Upload Failed ---");
        Serial.printf("Attempt: %d\n", uploadRetryCount);
        Serial.printf("Error: %s (code: %d)\n", 
                      aResult.error().message().c_str(), 
                      aResult.error().code());
        
        if (uploadRetryCount <= MAX_UPLOAD_RETRIES) {
            Serial.printf("Retry in: %d seconds\n", UPLOAD_RETRY_DELAY/1000);
            Serial.println("Status: RETRYING");
            lastUploadRetry = millis();
        } else {
            Serial.println("Status: FAILED (max retries)");
            uploadRetryCount = 0; // Reset for next upload cycle
        }
        
        Serial.println("---------------------");
        Serial.println();
        waitingForUpload = false;
    }
    
    if (aResult.available()) {
        Serial.println("--- Upload Success ---");
        
        if (uploadRetryCount > 0) {
            Serial.printf("Attempts: %d\n", uploadRetryCount + 1);
        } else {
            Serial.println("Attempts: 1");
        }
        
        // Mark first upload as completed
        if (!firstUploadDone) {
            firstUploadDone = true;
            Serial.println("Note: First upload completed");
        }
        
        Serial.println("Status: SUCCESS");
        Serial.println("---------------------");
        Serial.println();
        
        // Reset retry counter on successful upload
        uploadRetryCount = 0;
        waitingForUpload = false;
        aResult.clear();
    }
}

String getCurrentTimestamp()
{
    if (ntpInitialized) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char buffer[32];
            strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
            return String(buffer);
        }
    }
    
    // Fallback to system time if NTP is not available
    unsigned long currentTime = millis() / 1000;
    unsigned long hours = (currentTime / 3600) % 24;
    unsigned long minutes = (currentTime / 60) % 60;
    unsigned long seconds = currentTime % 60;
    
    char buffer[32];
    sprintf(buffer, "System Time %02lu:%02lu:%02lu", hours, minutes, seconds);
    return String(buffer);
}

String getCurrentDateTime()
{
    if (ntpInitialized) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char buffer[64];
            strftime(buffer, sizeof(buffer), "%d %B %Y at %H:%M:%S UTC+8", &timeinfo);
            return String(buffer);
        }
    }
    
    // Fallback to system time if NTP is not available
    unsigned long currentTime = millis() / 1000;
    unsigned long days = currentTime / 86400;
    unsigned long hours = (currentTime / 3600) % 24;
    unsigned long minutes = (currentTime / 60) % 60;
    unsigned long seconds = currentTime % 60;
    
    char buffer[64];
    sprintf(buffer, "System Day %lu at %02lu:%02lu:%02lu", days, hours, minutes, seconds);
    return String(buffer);
}

bool validateConfiguration()
{
    Serial.println("Validating configuration...");
    
    if (String(WIFI_SSID) == "WIFI_AP") {
        Serial.println("Please set your WiFi SSID");
        return false;
    }
    
    if (String(WIFI_PASSWORD) == "WIFI_PASSWORD") {
        Serial.println("Please set your WiFi password");
        return false;
    }
    
    if (String(FIREBASE_PROJECT_ID) == "PROJECT_ID") {
        Serial.println("Please set your Firebase project ID");
        return false;
    }
    
    Serial.println("Configuration validation passed!");
    return true;
}
