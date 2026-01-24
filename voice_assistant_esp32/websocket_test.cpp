#include <ArduinoWebsockets.h>
#include <WiFi.h>
using namespace websockets;
// Update these with your WiFi credentials
const char* ssid = "FLEMING_2";
const char* password = "90130762";
// WebSocket server
const char* websockets_server_host = "192.168.0.108:8000";
const uint16_t websockets_server_port = 80; // default for ws://, change if needed
const char* websocket_path = "/v1/realtime?model=deepdml/faster-whisper-large-v3-turbo-ct2&intent=transcription&api_key=speaches-test-api-key";
WebsocketsClient client;
void onMessageCallback(WebsocketsMessage message) {
    Serial.printf("Received text: %s\n", message.data().c_str());
}
void onBinaryCallback(WebsocketsMessage message) {
    Serial.println("Received binary data.");
    // Handle binary data if needed
}
void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    // Build ws://192.168.0.108/v1/realtime
    String ws_url = "ws://";
    ws_url += websockets_server_host;
    ws_url += websocket_path;
    // Optionally, add port if it's not 80 (for ws) or 443 (for wss)
    // ws_url = "ws://192.168.0.108:8080/v1/realtime";
    // Add handlers
    client.onMessage(onMessageCallback);
    client.onEvent([](WebsocketsEvent event, String data) {
        if (event == WebsocketsEvent::ConnectionOpened) {
            Serial.println("Conn open!");
        } else if (event == WebsocketsEvent::ConnectionClosed) {
            Serial.println("Conn closed!");
        } else if (event == WebsocketsEvent::GotPing) {
            Serial.println("Ping!");
        } else if (event == WebsocketsEvent::GotPong) {
            Serial.println("Pong!");
        }
    });
    // Connect (no headers by default)
    if (client.connect(ws_url)) {
        Serial.println("WebSocket connected!");
    } else {
        Serial.println("WebSocket connection failed!");
        while (1) delay(1000); // Stay here on fail
    }
}
void loop() {
    client.poll(); // Needed to keep connection alive and handle messages
}