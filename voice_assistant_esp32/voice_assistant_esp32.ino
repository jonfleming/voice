/*
* Voice Assistant for Freenove Media Kit ESP32-S3
* Integrates Wyoming Faster Whisper, Ollama, and Wyoming Piper via HTTP over WiFi
*/

#include "driver_audio_input.h"
#include "driver_audio_output.h"
#include "driver_button.h"
#include "driver_sdmmc.h" // Although not strictly needed for this version, keeping for reference
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // For parsing JSON responses

// Define the pin number for the button (do not modify)
#define BUTTON_PIN 19         
// Define the pin numbers for SDMMC interface (do not modify)
#define SD_MMC_CMD 38         
#define SD_MMC_CLK 39         
#define SD_MMC_D0 40          
// Define the pin numbers for audio input (do not modify)
#define AUDIO_INPUT_SCK 3     
#define AUDIO_INPUT_WS 14     
#define AUDIO_INPUT_DIN 46    
// Define the pin numbers for audio output (do not modify)
#define AUDIO_OUTPUT_BCLK 42  
#define AUDIO_OUTPUT_LRC 41   
#define AUDIO_OUTPUT_DOUT 1   

// Define the size of PSRAM in bytes for audio buffer
#define MOLLOC_SIZE (1024 * 1024) // 1MB buffer for audio recording

// ---------- WiFi / Server configuration (edit before upload) ----------
#define WIFI_SSID "FLEMING_2"
#define WIFI_PASS "90130762"
// The server that runs your transcription/TTS services (Raspberry Pi IP)
#define SERVER_IP "192.168.0.108"
#define WHISPER_PORT 8000
#define OLLAMA_PORT 11434
#define PIPER_PORT 8000
// ----------------------------------------------------------------------

// Audio settings (matching voice.py)
#define CHUNK_SIZE 1024 // Number of samples per buffer
#define SAMPLE_RATE 16000 // 16kHz
#define CHANNELS 1 // Mono
#define BITS_PER_SAMPLE 16 // 16-bit audio
#define SILENCE_THRESHOLD 500 // Adjust based on your environment
#define SILENCE_DURATION 2 // Seconds of silence to stop recording
#define RECORD_SECONDS 10 // Maximum recording time

// Global audio buffer in PSRAM
uint8_t *wav_buffer = NULL;
size_t total_recorded_size = 0;

// Button object
Button button(BUTTON_PIN);

// Task handles
TaskHandle_t voiceAssistantTaskHandle = NULL;

// Function prototypes
void wifi_connect();
void handle_button_events();
void start_voice_assistant_task();
void stop_voice_assistant_task();
void voice_assistant_task(void *pvParameters);
size_t record_audio_to_psram();
void play_audio_from_psram(uint8_t* audio_data, size_t data_len);
String transcribe_audio(uint8_t* audio_data, size_t data_len);
String get_ollama_response(const String& prompt);
void text_to_speech(const String& text);

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  Serial.println("ESP32-S3 Voice Assistant Starting...");

  button.init();
  audio_input_init(AUDIO_INPUT_SCK, AUDIO_INPUT_WS, AUDIO_INPUT_DIN);
  i2s_output_init(AUDIO_OUTPUT_BCLK, AUDIO_OUTPUT_LRC, AUDIO_OUTPUT_DOUT);

  // Allocate PSRAM for audio buffer
  wav_buffer = (uint8_t *)heap_caps_malloc(MOLLOC_SIZE, MALLOC_CAP_SPIRAM);
  if (wav_buffer == NULL) {
    Serial.println("Failed to allocate PSRAM for audio buffer!");
    while (true); // Halt if memory allocation fails
  }
  Serial.printf("PSRAM allocated: %d bytes\n", MOLLOC_SIZE);

  wifi_connect();

  Serial.println("\nPress the button to start recording.");
  Serial.println("Serial commands: (i)p info\n");
}

void loop() {
  button.key_scan();
  handle_button_events();

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'i') {
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
  }
  delay(10);
}

void wifi_connect() {
  Serial.printf("Connecting to WiFi SSID: %s\r\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - start > 20000) {
      Serial.println("\nWiFi connect timeout");
      return;
    }
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

void handle_button_events() {
  int button_state = button.get_button_state();
  int button_key_value = button.get_button_key_value();

  switch (button_key_value) {
    case 1: // Assuming button 1 is for record/stop
      if (button_state == Button::KEY_STATE_PRESSED) {
        start_voice_assistant_task();
      } else if (button_state == Button::KEY_STATE_RELEASED) {
        // stop_voice_assistant_task(); // We'll let silence detection stop it
      }
      break;
    default:
      break;
  }
}

void start_voice_assistant_task() {
  if (voiceAssistantTaskHandle == NULL) {
    Serial.println("Starting Voice Assistant Task...");
    xTaskCreate(voice_assistant_task, "VoiceAssistantTask", 8192, NULL, 1, &voiceAssistantTaskHandle);
  } else {
    Serial.println("Voice Assistant Task already running.");
  }
}

void stop_voice_assistant_task() {
  if (voiceAssistantTaskHandle != NULL) {
    Serial.println("Stopping Voice Assistant Task...");
    vTaskDelete(voiceAssistantTaskHandle);
    voiceAssistantTaskHandle = NULL;
  }
}

// Main Voice Assistant Task
void voice_assistant_task(void *pvParameters) {
  Serial.println("Voice Assistant Task started.");
  
  while (true) {
    Serial.println("Listening... (speak now)");
    total_recorded_size = record_audio_to_psram();

    if (total_recorded_size > 0) {
      Serial.printf("Recorded %d bytes of audio.\n", total_recorded_size);
      // Transcribe audio
      String transcribed_text = transcribe_audio(wav_buffer, total_recorded_size);
      if (transcribed_text.length() > 0) {
        Serial.printf("Transcribed text: %s\n", transcribed_text.c_str());

        // Get AI response
        String ai_response = get_ollama_response(transcribed_text);
        if (ai_response.length() > 0) {
          Serial.printf("AI Response: %s\n", ai_response.c_str());

          // Convert to speech and play
          text_to_speech(ai_response);

        } else {
          Serial.println("Failed to get AI response.");
        }

      } else {
        Serial.println("Transcription failed or no text detected.");
      }
    } else {
      Serial.println("No speech detected, trying again...");
    }
    
    Serial.println("\n==================================================\n");
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay before next loop iteration
  }
}

// Records audio from microphone until silence is detected or max time reached
size_t record_audio_to_psram() {
  size_t current_size = 0;
  int silent_chunks = 0;
  int silence_limit = (SILENCE_DURATION * SAMPLE_RATE) / CHUNK_SIZE;
  int max_chunks = (RECORD_SECONDS * SAMPLE_RATE) / CHUNK_SIZE;

  // Clear the buffer before recording
  memset(wav_buffer, 0, MOLLOC_SIZE);

  // Write WAV header to the buffer
  // This is a simplified header for 16-bit mono 16kHz WAV
  // Actual size will be filled in later
  uint32_t sample_rate = SAMPLE_RATE;
  uint16_t num_channels = CHANNELS;
  uint16_t bits_per_sample = BITS_PER_SAMPLE;

  // RIFF chunk
  memcpy(wav_buffer + 0, "RIFF", 4);
  *(uint32_t*)(wav_buffer + 4) = 0; // Placeholder for file size
  memcpy(wav_buffer + 8, "WAVE", 4);

  // FMT chunk
  memcpy(wav_buffer + 12, "fmt ", 4);
  *(uint32_t*)(wav_buffer + 16) = 16; // Subchunk1Size
  *(uint16_t*)(wav_buffer + 20) = 1;  // AudioFormat (1 for PCM)
  *(uint16_t*)(wav_buffer + 22) = num_channels;
  *(uint32_t*)(wav_buffer + 24) = sample_rate;
  *(uint32_t*)(wav_buffer + 28) = sample_rate * num_channels * (bits_per_sample / 8); // ByteRate
  *(uint16_t*)(wav_buffer + 32) = num_channels * (bits_per_sample / 8); // BlockAlign
  *(uint16_t*)(wav_buffer + 34) = bits_per_sample;

  // DATA chunk
  memcpy(wav_buffer + 36, "data", 4);
  *(uint32_t*)(wav_buffer + 40) = 0; // Placeholder for data size

  current_size = 44; // Start writing audio data after the header

  int16_t audio_chunk[CHUNK_SIZE]; // Buffer for I2S data

  for (int i = 0; i < max_chunks; ++i) {
    if (current_size + (CHUNK_SIZE * (BITS_PER_SAMPLE / 8)) >= MOLLOC_SIZE) {
      Serial.println("Audio buffer full, stopping recording.");
      break;
    }

    // Read I2S data
    size_t bytes_read = audio_input_read_iis_data((char*)audio_chunk, CHUNK_SIZE * (BITS_PER_SAMPLE / 8));
    if (bytes_read == 0) {
      Serial.println("No audio data read from I2S.");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Copy to PSRAM buffer
    memcpy(wav_buffer + current_size, (uint8_t*)audio_chunk, bytes_read);
    current_size += bytes_read;

    // Check for silence (simplified for 16-bit mono)
    long audio_level = 0;
    for (size_t j = 0; j < bytes_read / 2; ++j) { // Divide by 2 for 16-bit samples
      audio_level += abs(audio_chunk[j]);
    }
    audio_level /= (bytes_read / 2); // Average level

    if (audio_level < SILENCE_THRESHOLD) {
      silent_chunks++;
    } else {
      silent_chunks = 0;
    }

    if (silent_chunks > silence_limit) {
      Serial.println("Silence detected, stopping recording.");
      break;
    }
  }

  // Update WAV header with actual sizes
  size_t data_size = current_size - 44;
  *(uint32_t*)(wav_buffer + 4) = current_size - 8; // File size
  *(uint32_t*)(wav_buffer + 40) = data_size; // Data size

  return current_size;
}

// Plays audio from a given buffer
void play_audio_from_psram(uint8_t* audio_data, size_t data_len) {
  Serial.println("Playing audio...");
  // Assuming the audio_data is already a WAV format with header
  i2s_output_wav(audio_data, data_len);
  Serial.println("Playback complete.");
}

// Sends audio to Wyoming Faster Whisper for transcription
String transcribe_audio(uint8_t* audio_data, size_t data_len) {
  Serial.println("Transcribing audio with corrected HTTP POST...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi for transcription.");
    return "";
  }

  // Define boundary.
  String boundary = "---------------------------7da24f2e50046";

  // Construct the multipart form data parts
  String body_start = "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  body_start += "Content-Type: audio/wav\r\n\r\n";

  // Construct the final part of the multipart body in a single expression
  // to rule out any potential side effects from chained appends.
  String body_end = String("\r\n--") + boundary + "\r\n" +
                    "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
                    "Systran/faster-distil-whisper-small.en" +
                    String("\r\n--") + boundary + "--\r\n";

  // Calculate total payload size
  size_t total_payload_size = body_start.length() + data_len + body_end.length();
  
  // Allocate a buffer in PSRAM for the entire request body
  uint8_t *payload_buffer = (uint8_t *)heap_caps_malloc(total_payload_size, MALLOC_CAP_SPIRAM);
  if (payload_buffer == NULL) {
    Serial.println("Failed to allocate PSRAM for HTTP payload!");
    return "";
  }

  // Assemble the payload in the buffer
  size_t offset = 0;
  memcpy(payload_buffer + offset, body_start.c_str(), body_start.length());
  offset += body_start.length();
  memcpy(payload_buffer + offset, audio_data, data_len);
  offset += data_len;
  memcpy(payload_buffer + offset, body_end.c_str(), body_end.length());

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + String(WHISPER_PORT) + "/v1/audio/transcriptions";
  http.begin(url);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // Send the request with the complete payload
  int httpResponseCode = http.POST(payload_buffer, total_payload_size);

  String transcription = "";
  if (httpResponseCode > 0) {
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    String payload = http.getString();
    Serial.println("Response: " + payload);

    // Parse JSON response
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
    } else {
      transcription = doc["text"].as<String>();
      Serial.printf("Transcribed: %s\n", transcription.c_str());
    }
  } else {
    Serial.printf("HTTP POST failed, error: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
  heap_caps_free(payload_buffer); // Free the buffer
  return transcription;
}

// Sends prompt to Ollama and gets response
String get_ollama_response(const String& prompt) {
  Serial.println("Getting AI response...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi for Ollama.");
    return "";
  }

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + String(OLLAMA_PORT) + "/api/generate";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<1024> doc;
  doc["model"] = "llama3.2"; // OLLAMA_MODEL
  doc["prompt"] = prompt;
  doc["stream"] = false;

  String requestBody;
  serializeJson(doc, requestBody);

  int httpResponseCode = http.POST(requestBody);

  String ai_response = "";
  if (httpResponseCode > 0) {
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    String payload = http.getString();
    // Serial.println("Response: " + payload); // Uncomment for full response debugging

    StaticJsonDocument<2048> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, payload);

    if (error) {
      Serial.print(F("deserializeJson() failed for Ollama response: "));
      Serial.println(error.f_str());
    } else {
      ai_response = responseDoc["response"].as<String>();
      Serial.printf("AI Response: %s\n", ai_response.c_str());
    }
  } else {
    Serial.printf("HTTP POST failed for Ollama, error: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
  return ai_response;
}

// Sends text to Wyoming Piper for TTS and plays audio
void text_to_speech(const String& text) {
  Serial.println("Converting to speech...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi for TTS.");
    return;
  }

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + String(PIPER_PORT) + "/v1/audio/speech";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<512> doc;
  doc["model"] = "speaches-ai/Kokoro-82M-v1.0-ONNX"; // TTS_MODEL
  doc["voice"] = "af_heart"; // TTS_VOICE
  doc["input"] = text;

  String requestBody;
  serializeJson(doc, requestBody);

  int httpResponseCode = http.POST(requestBody);

  if (httpResponseCode > 0) {
    Serial.printf("HTTP Response code: %d\n", httpResponseCode);
    String contentType = http.header("Content-Type");

    if (contentType.startsWith("audio/wav")) {
      // Get audio data directly
      WiFiClient* client = http.getStreamPtr();
      size_t audio_len = http.getSize();

      if (audio_len > 0 && audio_len <= MOLLOC_SIZE) {
        // Read audio data into wav_buffer (reusing the recording buffer)
        size_t bytes_read = client->readBytes(wav_buffer, audio_len);
        if (bytes_read == audio_len) {
          play_audio_from_psram(wav_buffer, audio_len);
        } else {
          Serial.printf("Failed to read full audio data. Expected %d, got %d\n", audio_len, bytes_read);
        }
      } else if (audio_len > MOLLOC_SIZE) {
        Serial.printf("TTS audio too large for buffer (%d bytes). Max %d bytes.\n", audio_len, MOLLOC_SIZE);
      } else {
        Serial.println("TTS audio data length is 0.");
      }
    } else if (contentType.startsWith("application/json")) {
      String payload = http.getString();
      Serial.println("TTS returned JSON instead of audio: " + payload);
    } else {
      Serial.printf("Unexpected Content-Type from TTS: %s\n", contentType.c_str());
    }
  } else {
    Serial.printf("HTTP POST failed for TTS, error: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}
