/*
* Sketch_09_3_Reird_And_Play.ino
* This sketch records audio data from an audio input using the I2S bus and saves it as a WAV file on an SD card.
* It also plays back the recorded audio files using the same I2S bus.
* The recording and playback are controlled by a button press.
* 
* Author: Zhentao Lin
* Date:   2025-08-07
*/
#include "driver_audio_input.h"
#include "driver_audio_output.h"
#include "driver_button.h"
#include <esp_heap_caps.h>
// WiFi + HTTP
#include <WiFi.h>
#include <HTTPClient.h>

#define RECORDER_FOLDER ""
// Define the pin number for the button (do not modify)
#define BUTTON_PIN 19         
// (SD card support removed)
// Define the pin numbers for audio input (do not modify)
#define AUDIO_INPUT_SCK 3     
#define AUDIO_INPUT_WS 14     
#define AUDIO_INPUT_DIN 46    
// Define the pin numbers for audio output (do not modify)
#define AUDIO_OUTPUT_BCLK 42  
#define AUDIO_OUTPUT_LRC 41   
#define AUDIO_OUTPUT_DOUT 1   

// Define the size of PSRAM in bytes
#define MOLLOC_SIZE (1024 * 1024)

// Set to 1 to save recordings to SD card, 0 to keep in-memory and avoid SD writes
#define SAVE_TO_SD 0

// ---------- WiFi / Server configuration (edit before upload) ----------
#define WIFI_SSID "FLEMING_2"
#define WIFI_PASS "90130762"
// The server that runs your transcription/TTS services (Raspberry Pi IP)
#define SERVER_IP "192.168.0.108"
#define WHISPER_PORT 8000
#define OLLAMA_PORT 11434
#define PIPER_PORT 8000
// -----------------------------------------------------------------------

// Default STT model name (sent as form field `model`)
#define STT_MODEL "Systran/faster-distil-whisper-small.en"
// Ollama model to use for generation (change as needed)
#define OLLAMA_MODEL "llama3.2"
#define PIPER_MODEL "speaches-ai/Kokoro-82M-v1.0-ONNX"
#define PIPER_VOICE "af_heart"

// Create a button object with the specified pin
Button button(BUTTON_PIN);

// Flag to indicate the status of the recorder task (0=stopped, 1=running)
int recorder_task_flag = 0;       
// Flag to indicate the status of the player task (0=stopped, 1=running)
int player_task_flag = 0;        
// Save wav data
uint8_t *wav_buffer;
// Size of the last recorded buffer stored in PSRAM
size_t last_recorded_size = 0;

// Setup function to initialize the hardware and software components
void setup() {
  // Initialize the serial communication at 115200 baud rate
  Serial.begin(115200);
  // Wait for the serial port to be ready
  while (!Serial) {
    delay(10);
  }
  // Initialize the button
  button.init();

  // Initialize the I2S bus for audio input
  audio_input_init(AUDIO_INPUT_SCK, AUDIO_INPUT_WS, AUDIO_INPUT_DIN);
  // Initialize the I2S bus for audio output
  i2s_output_init(AUDIO_OUTPUT_BCLK, AUDIO_OUTPUT_LRC, AUDIO_OUTPUT_DOUT);

  // SD card support removed: using PSRAM-only recording
  Serial.println("SD card support disabled; using PSRAM-only buffers.");

  // Connect to WiFi (used for HTTP requests)
  wifi_connect();

  Serial.println("Serial commands: (t)est server, (i)p info\n");
}

// Main loop function that runs continuously
void loop() {
  // Scan the button state
  button.key_scan();
  // Handle button events
  handle_button_events();
  // Simple serial UI for testing WiFi / HTTP
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 't') {
      // Test a simple GET to the server root
      http_test_get();
    } else if (c == 'i') {
      // Print IP info
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());
    }
  }
  // Delay for 10 milliseconds
  delay(10);
}

// Connect to WiFi with simple retry logic
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

// Simple HTTP GET to the server root for a connectivity test
void http_test_get() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi");
    return;
  }

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + String(WHISPER_PORT) + "/";
  Serial.printf("GET %s\r\n", url.c_str());
  http.begin(url);
  int code = http.GET();
  if (code > 0) {
    Serial.printf("HTTP code: %d\r\n", code);
    String payload = http.getString();
    Serial.println("Response (truncated to 1024 chars):");
    if (payload.length() > 1024) payload = payload.substring(0, 1024);
    Serial.println(payload);
  } else {
    Serial.printf("HTTP GET failed, error: %s\r\n", http.errorToString(code).c_str());
  }
  http.end();
}

// Helper: write 44-byte WAV header to the given client for PCM32, stereo as configured
void write_wav_header_to_client(WiFiClient &client, uint32_t data_bytes) {
  uint32_t sample_rate = 32000; // match hardware sample rate
  uint16_t channels = 2;
  uint16_t bits_per_sample = 32;

  uint32_t byte_rate = sample_rate * channels * bits_per_sample / 8;
  uint16_t block_align = channels * bits_per_sample / 8;
  uint32_t subchunk2_size = data_bytes;
  uint32_t chunk_size = 36 + subchunk2_size;

  // RIFF header
  client.write((const uint8_t *)"RIFF", 4);
  client.write((const uint8_t *)&chunk_size, 4);
  client.write((const uint8_t *)"WAVE", 4);

  // fmt subchunk
  client.write((const uint8_t *)"fmt ", 4);
  uint32_t subchunk1_size = 16;
  client.write((const uint8_t *)&subchunk1_size, 4);
  uint16_t audio_format = 1; // PCM
  client.write((const uint8_t *)&audio_format, 2);
  client.write((const uint8_t *)&channels, 2);
  client.write((const uint8_t *)&sample_rate, 4);
  client.write((const uint8_t *)&byte_rate, 4);
  client.write((const uint8_t *)&block_align, 2);
  client.write((const uint8_t *)&bits_per_sample, 2);

  // data subchunk
  client.write((const uint8_t *)"data", 4);
  client.write((const uint8_t *)&subchunk2_size, 4);
}

// Simple JSON value extractor for top-level string fields (naive, but fine for small predictable responses)
String extract_json_string_value(const String &json, const String &key) {
  String needle = String("\"") + key + String("\"") + String(":");
  int idx = json.indexOf(needle);
  if (idx < 0) return String("");
  // move to first quote after ':'
  int q = json.indexOf('"', idx + needle.length());
  if (q < 0) return String("");
  int q2 = q + 1;
  String out = "";
  while (q2 < json.length()) {
    char c = json[q2];
    if (c == '"') break;
    // handle basic escapes
    if (c == '\\' && q2 + 1 < json.length()) {
      char esc = json[q2 + 1];
      if (esc == '"') out += '"';
      else if (esc == 'n') out += '\n';
      else if (esc == 'r') out += '\r';
      else if (esc == 't') out += '\t';
      else out += esc;
      q2 += 2;
      continue;
    }
    out += c;
    q2++;
  }
  return out;
}

// Extract all occurrences of a string field (useful for streaming JSON lines)
String extract_all_json_string_values(const String &json, const String &key) {
  String out = "";
  String needle = String("\"") + key + String("\"") + String(":");
  int start = 0;
  while (true) {
    int idx = json.indexOf(needle, start);
    if (idx < 0) break;
    // move to first quote after ':'
    int q = json.indexOf('"', idx + needle.length());
    if (q < 0) break;
    int q2 = q + 1;
    while (q2 < json.length()) {
      char c = json[q2];
      if (c == '"') break;
      if (c == '\\' && q2 + 1 < json.length()) {
        char esc = json[q2 + 1];
        if (esc == '"') out += '"';
        else if (esc == 'n') out += '\n';
        else if (esc == 'r') out += '\r';
        else if (esc == 't') out += '\t';
        else out += esc;
        q2 += 2;
        continue;
      }
      out += c;
      q2++;
    }
    // advance search position
    start = q2 + 1;
  }
  return out;
}

// Minimal JSON string escaper for safe embedding in request bodies
String json_escape(const String &s) {
  String out = "";
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

// Send the transcription text to the Ollama server and print the response
void send_transcription_to_ollama(const String &text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi, cannot send to Ollama");
    return;
  }
  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + String(OLLAMA_PORT) + "/api/generate";
  Serial.printf("POST %s\r\n", url.c_str());
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String body = String("{\"model\":\"") + OLLAMA_MODEL + String("\",\"prompt\":\"") + json_escape(text) + String("\"}");

  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("Ollama HTTP code: %d\r\n", code);
    String resp = http.getString();
    Serial.println("Ollama response:");
    Serial.println(resp);
    // Try to extract a usable text response from Ollama JSON.
    // Ollama may stream many small JSON objects; aggregate any "response" fields.
    String ai_text = extract_all_json_string_values(resp, "response");
    if (ai_text.length() == 0) ai_text = extract_all_json_string_values(resp, "content");
    if (ai_text.length() == 0) ai_text = extract_json_string_value(resp, "text");
    ai_text.trim();
    if (ai_text.length() > 0) {
      Serial.println("Parsed AI output:");
      Serial.println(ai_text);
      // Send parsed AI text to Piper for TTS
      send_text_to_piper(ai_text);
    } else {
      Serial.println("Could not parse AI text from Ollama response.");
    }
  } else {
    Serial.printf("Ollama POST failed, error: %s\r\n", http.errorToString(code).c_str());
  }
  http.end();
}

// Send text to Piper (Wyoming Piper compatible) for TTS and play the returned WAV
void send_text_to_piper(const String &text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi for TTS.");
    return;
  }

  HTTPClient http;
  String url = String("http://") + SERVER_IP + ":" + String(PIPER_PORT) + "/v1/audio/speech";
  Serial.printf("POST %s\r\n", url.c_str());
  http.begin(url);
  // Ask HTTPClient to collect these response headers so http.header() works after the request
  const char* responseHeaders[] = { "Content-Type", "Content-Length" };
  http.collectHeaders(responseHeaders, 2);
  http.addHeader("Content-Type", "application/json");

  // Lightweight defaults — change model/voice as needed
  String tts_model = PIPER_MODEL;
  String tts_voice = PIPER_VOICE;

  String body = String("{\"model\":\"") + tts_model + String("\",\"voice\":\"") + tts_voice + String("\",\"input\":\"") + json_escape(text) + String("\",\"response_format\":\"wav\"}");

  // For debugging, print first 50 character of text
  String text50 = text.length() > 50 ? text.substring(0, 50) + "..." : text;
  String debugBody = String("{\"model\":\"") + tts_model + String("\",\"voice\":\"") + tts_voice + String("\",\"input\":\"") + json_escape(text50) + String("\",\"response_format\":\"wav\"}");
  Serial.printf("TTS request body (truncated): %s\r\n", debugBody.c_str());

  int code = http.POST(body);
  if (code > 0) {
    Serial.printf("Piper HTTP code: %d\r\n", code);
    String contentType = http.header("Content-Type");
    String headerContentLength = http.header("Content-Length");
    int contentLenReported = http.getSize();
    Serial.printf("Response headers: Content-Type='%s', Content-Length(header)='%s', getSize()=%d\r\n",
      contentType.c_str(), headerContentLength.c_str(), contentLenReported);
    // Normalize header check
    if (contentType.length() == 0) {
      Serial.println("Warning: Content-Type header is empty from Piper.");
    }

    if (contentType.indexOf("audio/") == 0 || contentType.indexOf("audio") >= 0) {
      // Read raw audio bytes from the stream
      WiFiClient *client = http.getStreamPtr();
      int audio_len = http.getSize();
      size_t bytes_read = 0;

      // Ensure wav_buffer is allocated before attempting to read streamed audio into it
      if (wav_buffer == NULL) {
        wav_buffer = (uint8_t *)heap_caps_malloc(MOLLOC_SIZE, MALLOC_CAP_SPIRAM);
        if (wav_buffer == NULL) {
          Serial.println("ERROR: failed to allocate PSRAM buffer for TTS playback.");
          http.end();
          return;
        }
      }

      if (audio_len > 0 && audio_len <= MOLLOC_SIZE) {
        bytes_read = client->readBytes(wav_buffer, audio_len);
      } else {
        // Unknown size or larger than buffer: stream up to MOLLOC_SIZE
        size_t to_read = 0;
        while (client->connected() || client->available()) {
          if (client->available()) {
            size_t remaining = MOLLOC_SIZE - to_read;
            if (remaining == 0) break;
            size_t want = remaining > 1024 ? 1024 : remaining;
            int chunk = client->readBytes((char*)wav_buffer + to_read, (size_t)want);
            if (chunk <= 0) break;
            to_read += (size_t)chunk;
            if (to_read >= MOLLOC_SIZE) break;
          } else {
            delay(5);
          }
        }
        bytes_read = to_read;
      }

      if (bytes_read > 0) {
        Serial.printf("Received TTS audio bytes: %d\r\n", bytes_read);
        // Dump first bytes for debugging
        int dump_n = bytes_read < 64 ? bytes_read : 64;
        Serial.print("First "); Serial.print(dump_n); Serial.println(" bytes (hex):");
        for (int i = 0; i < dump_n; ++i) {
          Serial.printf("%02x ", wav_buffer[i]);
        }
        Serial.println();
        Serial.println("First bytes (ASCII, non-printable shown as '.'):");
        for (int i = 0; i < dump_n; ++i) {
          char c = (char)wav_buffer[i];
          if (c >= 32 && c <= 126) Serial.print(c); else Serial.print('.');
        }
        Serial.println();

        i2s_output_wav(wav_buffer, bytes_read);
      } else {
        Serial.println("No TTS audio received or failed to read audio.");
      }
    } else if (contentType.indexOf("application/json") >= 0) {
      String payload = http.getString();
      Serial.println("TTS returned JSON instead of audio: ");
      Serial.println(payload);
    } else {
      Serial.printf("Unexpected Content-Type from TTS: %s\r\n", contentType.c_str());
      // For debugging and fallback: read the entire response stream into wav_buffer
      WiFiClient *clientDbg = http.getStreamPtr();
      if (clientDbg) {
        Serial.printf("Stream pointer available. client->available()=%d\r\n", clientDbg->available());
        size_t to_read = 0;
        unsigned long start_ms = millis();
        while (clientDbg->connected() || clientDbg->available()) {
          if (clientDbg->available()) {
            size_t remaining = MOLLOC_SIZE - to_read;
            if (remaining == 0) break;
            size_t want = remaining > 1024 ? 1024 : remaining;
            int chunk = clientDbg->readBytes((char*)wav_buffer + to_read, (size_t)want);
            if (chunk <= 0) break;
            to_read += (size_t)chunk;
            if (to_read >= MOLLOC_SIZE) break;
          } else {
            if (millis() - start_ms > 5000) break; // timeout waiting for data
            delay(5);
          }
        }
        Serial.printf("Read %u bytes for debug dump\r\n", (unsigned)to_read);

        if (to_read >= 4 && wav_buffer[0] == 'R' && wav_buffer[1] == 'I' && wav_buffer[2] == 'F' && wav_buffer[3] == 'F') {
          Serial.println("Detected RIFF header in response — treating as WAV audio.");
          int dump_n = to_read < 64 ? to_read : 64;
          Serial.print("First "); Serial.print(dump_n); Serial.println(" bytes (hex):");
          for (int i = 0; i < dump_n; ++i) Serial.printf("%02x ", wav_buffer[i]);
          Serial.println();
          Serial.println("First bytes (ASCII, non-printable shown as '.'):");
          for (int i = 0; i < dump_n; ++i) { char c = (char)wav_buffer[i]; if (c >= 32 && c <= 126) Serial.print(c); else Serial.print('.'); }
          Serial.println();
          i2s_output_wav(wav_buffer, to_read);
        } else if (to_read > 0) {
          // Fallback: show a shorter debug dump
          int dump_n = to_read < 128 ? to_read : 128;
          Serial.println("Debug dump (hex):");
          for (int i = 0; i < dump_n; ++i) Serial.printf("%02x ", wav_buffer[i]);
          Serial.println();
          Serial.println("Debug dump (ASCII):");
          for (int i = 0; i < dump_n; ++i) { char c = (char)wav_buffer[i]; if (c >= 32 && c <= 126) Serial.print(c); else Serial.print('.'); }
          Serial.println();
        } else {
          Serial.println("No bytes available on stream for debug dump.");
        }
      } else {
        Serial.println("No stream pointer available for debug dump.");
      }
    }
  } else {
    Serial.printf("Piper POST failed, error: %s\r\n", http.errorToString(code).c_str());
  }
  http.end();
}

// Stream a WAV (header + raw PCM in PSRAM) as multipart/form-data to the transcription server
void post_wav_stream_psram(const char *model, uint8_t *buffer, size_t length) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to WiFi, cannot POST");
    return;
  }

  const char *host = SERVER_IP;
  uint16_t port = WHISPER_PORT;
  const char *path = "/v1/audio/transcriptions"; // endpoint used by Wyoming Faster-Whisper

  String boundary = "----ESP32Boundary" + String(millis());

  // Build multipart preamble (model field + file headers). We'll send WAV header and PCM after this.
  String part_model = String("--") + boundary + "\r\n";
  part_model += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  part_model += String(model) + "\r\n";

  String part_file_header = String("--") + boundary + "\r\n";
  part_file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n";
  part_file_header += "Content-Type: audio/wav\r\n\r\n";

  String part_end = "\r\n" + String("--") + boundary + "--\r\n";

  // Calculate content-length: model part + file header + WAV header (44) + data + trailing boundary
  uint32_t wav_header_size = 44;
  uint32_t content_length = part_model.length() + part_file_header.length() + wav_header_size + length + part_end.length();

  WiFiClient client;
  Serial.printf("Connecting to %s:%d\r\n", host, port);
  if (!client.connect(host, port)) {
    Serial.println("Connection failed");
    return;
  }

  // Send request headers
  client.print(String("POST ") + path + " HTTP/1.1\r\n");
  client.print(String("Host: ") + host + ":" + port + "\r\n");
  client.print("User-Agent: ESP32\r\n");
  client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  client.print(String("Content-Length: ") + content_length + "\r\n");
  client.print("Connection: close\r\n\r\n");

  // Send multipart body: model part
  client.print(part_model);
  // Send file header
  client.print(part_file_header);

  // Send WAV header
  write_wav_header_to_client(client, length);

  // Send PCM data from PSRAM in chunks
  size_t sent = 0;
  const size_t CHUNK = 1024;
  while (sent < length) {
    size_t to_send = (length - sent) > CHUNK ? CHUNK : (length - sent);
    client.write(buffer + sent, to_send);
    sent += to_send;
    // optional small yield to avoid watchdog
    yield();
  }

  // Send ending boundary
  client.print(part_end);

  Serial.println("Request sent, waiting for response...");

  // Read response
  unsigned long timeout = millis() + 10000; // 10s timeout
  String response = "";
  while (client.connected() || client.available()) {
    while (client.available()) {
      char c = client.read();
      response += c;
    }
    if (millis() > timeout) break;
    delay(5);
  }
  client.stop();

  // Truncate large response for logging
  if (response.length() > 8192) response = response.substring(response.length() - 8192);

  // Find JSON body start (first '{')
  int json_start = response.indexOf('{');
  String json = json_start >= 0 ? response.substring(json_start) : response;

  Serial.println("Server response (body):");
  Serial.println(json);

  // Extract `text` field and print
  String text = extract_json_string_value(json, "text");
  if (text.length()) {
    Serial.println("Transcription:");
    Serial.println(text);
    // Send transcription to Ollama
    send_transcription_to_ollama(text);
  } else {
    Serial.println("No transcription found in response.");
  }
}

/*
  Integration notes / next steps (how to proceed):
  - After recording finishes and you have the WAV in `wav_buffer` and `total_size`:
    - You can POST the WAV to the transcription server as multipart/form-data.
    - On ESP32 you should stream from SD or PSRAM in chunks to avoid transient OOM.
  - Suggested flow to implement next:
    1. Build a multipart/form-data POST that includes the file field named "file" and the "model" param.
    2. Use WiFiClient + HTTPClient to stream the body so you don't allocate a second buffer.
    3. Parse the JSON response and print the `text` field to Serial.
    4. Send that text to Ollama's /api/generate and parse the response.
    5. Request TTS from Piper and call `i2s_output_wav()` with the returned WAV bytes.

  I can add a streaming multipart example next if you'd like — tell me whether you
  prefer the ESP to stream directly from PSRAM or from the SD file on your board.
*/

// Function to handle button events
void handle_button_events() {
  // Get the current state of the button
  int button_state = button.get_button_state();
  // Get the key value associated with the button press
  int button_key_value = button.get_button_key_value();
  // Switch case based on the button key value
  switch (button_key_value) {
    case 1:
      // If the button is pressed, start the recorder task
      if (button_state == Button::KEY_STATE_PRESSED) {
        start_recorder_task();
      } 
      // If the button is released, stop the recorder task
      else if (button_state == Button::KEY_STATE_RELEASED) {
        stop_recorder_task();
      }
      break;
    case 2:
      // If the button is pressed, start the player task
      if (button_state == Button::KEY_STATE_PRESSED) {
        start_player_task();
      }
      break;
    case 3:
      // If the button is pressed, stop the player task
      if (button_state == Button::KEY_STATE_PRESSED) {
        stop_player_task();
      }
    case 4:
    case 5:
    default:
      // Default case for other button key values
      break;
  }
}

/* Start recording task */
void start_recorder_task(void) {
  // Check if the recorder task is not already running
  if (recorder_task_flag == 0) {
    // Set the recorder task flag to running
    recorder_task_flag = 1;
    // Create a new task for recording sound
    xTaskCreate(loop_task_sound_recorder, "loop_task_sound_recorder", 4096, NULL, 1, NULL);
  }
}

/* Stop recording task */
void stop_recorder_task(void) {
  // Check if the recorder task is running
  if (recorder_task_flag == 1) {
    // Set the recorder task flag to stopped
    recorder_task_flag = 0;
    // Print a message indicating the deletion of the recorder task
    Serial.println("loop_task_sound_recorder deleted!");
  }
}

/* Check if recording task is active */
int is_recorder_task_running(void) {
  // Return the status of the recorder task
  return recorder_task_flag;
}

/* Main recording task loop */
void loop_task_sound_recorder(void *pvParameters) {
  // Print a message indicating the start of the recording task
  Serial.println("loop_task_sound_recorder start...");
  // Initialize the total size of recorded data
  int total_size = 0;
  // Allocate memory in PSRAM for storing audio data
  if(wav_buffer!=NULL)
  {
    // Free the allocated memory in PSRAM
    heap_caps_free(wav_buffer);
    wav_buffer = NULL;
  }
  wav_buffer = (uint8_t *)heap_caps_malloc(MOLLOC_SIZE, MALLOC_CAP_SPIRAM);

  // No filesystem: record directly into PSRAM buffer

  // Loop while the recorder task is running
  while (recorder_task_flag == 1) {
    // Get the available IIS data size
    int iis_buffer_size = audio_input_get_iis_data_available();
    // Loop while there is IIS data available
    while (iis_buffer_size > 0) {
      // Check if the buffer is full
      if ((total_size + 512) >= MOLLOC_SIZE) {
        // Stop the recorder task if the buffer is full
        recorder_task_flag = 0;
        break;
      }
      // Read IIS data into the buffer
      int real_size = audio_input_read_iis_data((char*)wav_buffer + total_size, 512);
      // Update the total size of recorded data
      total_size += real_size;
      // Decrease the available IIS data size
      iis_buffer_size -= real_size;
    }
  }
  // Write the WAV header to the file and append data only when saving to SD
  // Save the final recorded size for in-memory playback and downstream processing
  last_recorded_size = total_size;
  Serial.printf("Recorded bytes in PSRAM: %u\r\n", (unsigned)last_recorded_size);
  
  Serial.printf("write wav size:%d\r\n", total_size);
  // Stream the recorded WAV (header + PSRAM buffer) to the transcription server
  post_wav_stream_psram(STT_MODEL, wav_buffer, total_size);
  // Print a message indicating the end of the recording task
  Serial.println("loop_task_sound_recorder stop...");
  // Delete the current task
  vTaskDelete(NULL);
}

/* Start player task */
void start_player_task(void) {
  // Check if the player task is not already running
  if (player_task_flag == 0) {
    // Set the player task flag to running
    player_task_flag = 1;
    // Create a new task for playing sound
    xTaskCreate(loop_task_play_handle, "loop_task_play_handle", 4096, NULL, 1, NULL);
  }
}

/* Stop player task */
void stop_player_task(void) {
  // Check if the player task is running
  if (player_task_flag == 1) {
    // Set the player task flag to stopped
    player_task_flag = 0;
    // Print a message indicating the deletion of the player task
    Serial.println("loop_task_play_handle deleted!");
  }
}

/* Check if player task is active */
int is_player_task_running(void) {
  // Return the status of the player task
  return player_task_flag;
}

/* Main player task loop */
void loop_task_play_handle(void *pvParameters) {
  // Print a message indicating the start of the player task
  Serial.println("loop_task_play_handle start...");
  // Loop while the player task is running
  while (player_task_flag == 1) {
      // Play the last in-memory recording (PSRAM)
      if (wav_buffer != NULL && last_recorded_size > 0) {
        Serial.printf("Playing in-memory recording, size=%u\r\n", (unsigned)last_recorded_size);
        i2s_output_wav(wav_buffer, last_recorded_size);
      } else {
        Serial.println("No in-memory recording available to play.");
      }
      player_task_flag = 0;
  }
  // Print a message indicating the end of the player task
  Serial.println("loop_task_play_handle stop...");
  // Delete the current task
  vTaskDelete(NULL);
}

















