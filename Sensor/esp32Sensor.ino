

#define ENABLE_FIRESTORE

#include <FirebaseClient.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include <time.h>
#include "ExampleFunctions.h"

// WiFi and Firebase Configuration
#define WIFI_SSID "ICT-LAB"
#define WIFI_PASSWORD "Student@sit"
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
#define MAX_READING_ID_SEARCH 10000  // Maximum reading ID to search (prevents infinite loops)
#define READING_ID_SEARCH_TIMEOUT 30000 // 30 seconds timeout for reading ID search
#define MAX_UPLOAD_RETRIES 5         // Maximum number of upload retry attempts (more aggressive)
#define UPLOAD_RETRY_DELAY 3000      // Delay between upload retries (3 seconds - more aggressive)

// Function declarations
void processFirebaseData(AsyncResult &aResult);
void processReadingCheckData(AsyncResult &aResult);
void readModbusSensors();
void sendSensorDataToFirebase();
void findNextAvailableReadingId();
String getCurrentTimestamp();
String getCurrentDateTime();
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
AsyncResult readingCheckResult;

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
unsigned long readingIdSearchStart = 0;
int currentReadingId = 0;        // Current ID we're checking
int nextReadingId = -1;          // -1 means not found yet
bool configValid = false;
bool modbusInitialized = false;
bool wifiInitialized = false;
bool ntpInitialized = false;
bool readingIdFound = false;
bool waitingForReadingCheck = false;
bool waitingForUpload = false;
bool searchingForNextId = false;
bool firebaseInitialized = false;
int uploadRetryCount = 0;             // Current retry attempt count
unsigned long lastUploadRetry = 0;    // Timestamp of last retry attempt
bool firstUploadDone = false;         // Track if first upload has been completed

void setup()
{
    Serial.begin(115200);
    delay(2000);
    
    Serial.println("=== Modbus Sensor to Firebase (Auto-increment) ===");
    
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
            readingIdSearchStart = millis();
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
    
    // Find next available reading ID if not found yet
    if (firebaseInitialized && app.ready() && !readingIdFound && !waitingForReadingCheck && !searchingForNextId) {
        debugPrint("Starting search for next available reading ID...");
        findNextAvailableReadingId();
    }
    
    // Handle reading ID search timeout
    if (searchingForNextId && (millis() - readingIdSearchStart) > READING_ID_SEARCH_TIMEOUT) {
        debugPrint("Reading ID search timeout - using fallback method");
        nextReadingId = random(1000, 9999); // Use random ID as fallback
        readingIdFound = true;
        searchingForNextId = false;
        waitingForReadingCheck = false;
        Serial.printf("Using fallback reading ID: %d\n", nextReadingId);
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
        debugPrint("Reading ID found: " + String(readingIdFound ? "YES" : "NO"));
        debugPrint("Next reading ID: " + String(nextReadingId));
        debugPrint("Waiting for upload: " + String(waitingForUpload ? "YES" : "NO"));
        debugPrint("Time since last Firebase send: " + String(millis() - lastFirebaseSend) + "ms");
        debugPrint("==================");
    }
    
    // Send data to Firebase every 1 minute (only if we have valid sensor data and reading ID)
    // For the first upload, skip the timer check to upload immediately
    bool timeConditionMet = firstUploadDone ? (millis() - lastFirebaseSend >= FIREBASE_SEND_INTERVAL) : true;
    
    if (firebaseInitialized && app.ready() && currentSensorData.isValid && readingIdFound && 
        nextReadingId >= 0 && !waitingForUpload && timeConditionMet) {
        
        debugPrint("All conditions met - attempting Firebase upload...");
        uploadRetryCount = 0; // Reset retry count for new upload
        
        if (!firstUploadDone) {
            Serial.println("Starting first upload (no delay)");
        }
        
        Serial.printf("Uploading reading_%d...\n", nextReadingId);
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
    processReadingCheckData(readingCheckResult);
    
    delay(100);
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

void findNextAvailableReadingId()
{
    if (searchingForNextId) {
        return; // Already searching
    }
    
    debugPrint("Checking if reading_" + String(currentReadingId) + " exists...");
    searchingForNextId = true;
    waitingForReadingCheck = true;
    readingIdSearchStart = millis();
    
    // Check if document exists
    String documentPath = "sensor_data/reading_";
    documentPath += currentReadingId;
    
    Docs.get(aClient, Firestore::Parent(FIREBASE_PROJECT_ID), documentPath, GetDocumentOptions(), readingCheckResult);
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

void sendSensorDataToFirebase()
{
    debugPrint("Attempting to upload to Firebase...");
    
    // Show current retry status
    if (uploadRetryCount > 0) {
        Serial.printf("Retry attempt %d/%d for reading_%d\n", 
                     uploadRetryCount + 1, MAX_UPLOAD_RETRIES + 1, nextReadingId);
    }
    
    waitingForUpload = true;
    
    // Create document path: sensor_data/reading_X
    String documentPath = "sensor_data/reading_";
    documentPath += nextReadingId;
    
    Serial.printf("Creating document: %s\n", documentPath.c_str());
    
    // Create document with only the requested fields
    Document<Values::Value> doc("timestamp", Values::Value(Values::StringValue(getCurrentTimestamp())));
    doc.add("datetime", Values::Value(Values::StringValue(getCurrentDateTime())));
    doc.add("temperature", Values::Value(Values::DoubleValue(currentSensorData.temperature)));
    doc.add("humidity", Values::Value(Values::DoubleValue(currentSensorData.humidity)));
    doc.add("pressure", Values::Value(Values::DoubleValue(currentSensorData.pressure)));
    
    // Send to Firebase
    debugPrint("Sending document to Firebase...");
    Docs.createDocument(aClient, Firestore::Parent(FIREBASE_PROJECT_ID), documentPath, DocumentMask(), doc, firestoreResult);
}

void processFirebaseData(AsyncResult &aResult)
{
    if (!aResult.isResult()) return;
    
    if (aResult.isError()) {
        // Handle retry logic
        uploadRetryCount++;
        
        Serial.println("--- Upload Failed ---");
        Serial.printf("Document: reading_%d\n", nextReadingId);
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
        Serial.printf("Document: reading_%d\n", nextReadingId);
        
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
        
        // Move to next reading ID for next upload
        nextReadingId++;
        
        // Reset retry counter on successful upload
        uploadRetryCount = 0;
        waitingForUpload = false;
        aResult.clear();
    }
}

void processReadingCheckData(AsyncResult &aResult)
{
    if (!aResult.isResult()) return;
    
    waitingForReadingCheck = false;
    searchingForNextId = false;
    
    if (aResult.isError()) {
        int errorCode = aResult.error().code();
        if (errorCode == 404) {
            // Document doesn't exist - this is our next available ID!
            debugPrint("Found next available reading ID: " + String(currentReadingId));
            nextReadingId = currentReadingId;
            readingIdFound = true;
        } else {
            Serial.printf("Error checking reading_%d: %s (code: %d)\n", 
                          currentReadingId, 
                          aResult.error().message().c_str(), 
                          aResult.error().code());
            
            // If we get an error that's not 404, wait and try again
            delay(1000);
        }
        Serial.println();
    } else if (aResult.available()) {
        // Document exists, try the next ID
        debugPrint("reading_" + String(currentReadingId) + " exists, checking next...");
        currentReadingId++;
        
        // Safety check to prevent infinite loops
        if (currentReadingId > MAX_READING_ID_SEARCH) {
            Serial.println("Reached maximum reading ID search limit!");
            Serial.printf("Using reading ID: %d\n", currentReadingId);
            nextReadingId = currentReadingId;
            readingIdFound = true;
        }
        
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