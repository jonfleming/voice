/*
* Sketch_09_3_Reird_And_Play.ino
* This sketch records audio data from an audio input using the I2S bus, sends it to a server for transcription,
* receives the transcribed text, sends it to an AI model for generating a response, and then uses a TTS service
* to convert the response text back into speech, which is played through an audio output using I2S.
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
// Display
#include "display.h"
#include <lvgl.h>
#include <freertos/semphr.h>
#include <math.h>

// Mutex to protect display request buffers
SemaphoreHandle_t display_mutex = NULL;

#define RECORDER_FOLDER ""
// Define the pin number for the button (do not modify)
#define BUTTON_PIN 19         
// Define the pin numbers for audio input (do not modify)
#define AUDIO_INPUT_SCK 3     
#define AUDIO_INPUT_WS 14     
#define AUDIO_INPUT_DIN 46    
// Define the pin numbers for audio output (do not modify)
#define AUDIO_OUTPUT_BCLK 42  
#define AUDIO_OUTPUT_LRC 41   
#define AUDIO_OUTPUT_DOUT 1   

// Define the size of PSRAM in bytes
#define MOLLOC_SIZE (4 * 1024 * 1024)

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

// Global button instance is declared in `driver_button.h` and defined in `display.cpp`.
// Use the shared `button` instance (defined in display.cpp) via the extern declaration.

// Save wav data
uint8_t *wav_buffer;
// Size of the last recorded buffer stored in PSRAM
size_t last_recorded_size = 0;

// VAD parameters
#define VAD_WINDOW_SIZE 128  // number of stereo pairs to process (256 left samples)
#define VAD_THRESHOLD 3000   // threshold for mean abs value (tune this)
#define VAD_LOW_COUNT 5      // number of low energy windows to stop recording

// VAD state
int vad_low_energy_count = 0;
int vad_samples = 0;

// Task handles for state control
// If handle is NULL, the task/feature is inactive; non-NULL means active
TaskHandle_t vad_task_handle_internal = NULL;  // The actual VAD task handle
volatile TaskHandle_t vad_task_handle = NULL;  // NULL = VAD disabled, non-NULL = VAD enabled
volatile TaskHandle_t recorder_task_handle = NULL;  // NULL = not recording, non-NULL = recording
volatile TaskHandle_t player_task_handle = NULL;    // NULL = not playing, non-NULL = playing

// Thread-safe display request buffers (background tasks must never call LVGL directly)
char display_line1_buf[128] = {0};
volatile bool display_line1_pending = false;
char display_line2_buf[128] = {0};
volatile bool display_line2_pending = false;

// Boot instruction requests
char display_boot_buf[128] = {0};
volatile bool display_boot_show_pending = false;
volatile bool display_boot_hide_pending = false;
// Request to clear both display lines (processed on main loop)
volatile bool display_clear_pending = false;

void request_showBootInstructions(const char *text) {
  if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
  strncpy(display_boot_buf, text, sizeof(display_boot_buf)-1);
  display_boot_buf[sizeof(display_boot_buf)-1] = '\0';
  display_boot_show_pending = true;
  display_boot_hide_pending = false;
  if (display_mutex) xSemaphoreGive(display_mutex);
}

void request_hideBootInstructions() {
  if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
  display_boot_hide_pending = true;
  display_boot_show_pending = false;
  if (display_mutex) xSemaphoreGive(display_mutex);
}

// Request to clear display lines from background tasks
void request_clear_lines() {
  if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
  display_clear_pending = true;
  if (display_mutex) xSemaphoreGive(display_mutex);
}

// Request a main-loop display update for line1
void request_display_line1(const char *text) {
  if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
  strncpy(display_line1_buf, text, sizeof(display_line1_buf)-1);
  display_line1_buf[sizeof(display_line1_buf)-1] = '\0';
  display_line1_pending = true;
  if (display_mutex) xSemaphoreGive(display_mutex);
}

// Request a main-loop display update for line2
void request_display_line2(const char *text) {
  if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
  strncpy(display_line2_buf, text, sizeof(display_line2_buf)-1);
  display_line2_buf[sizeof(display_line2_buf)-1] = '\0';
  display_line2_pending = true;
  if (display_mutex) xSemaphoreGive(display_mutex);
}

// Persistent WiFi clients for HTTP keep-alive
WiFiClient whisper_client;

// Track last debounced button state to detect edges
int last_button_state_for_toggle = Button::KEY_STATE_IDLE;

// Setup function to initialize the hardware and software components
void setup() {
  // Initialize the serial communication at 115200 baud rate
  Serial.begin(115200);
  // Wait for the serial port to be ready
  while (!Serial) {
    delay(10);
  }
  // Display
  display.init(TFT_DIRECTION);
  // Show boot instruction at top of screen
  // Prompt user to enable VAD via the button
  display.showBootInstructions("Press button to start VAD");

  // Initialize the I2S bus for audio input
  audio_input_init(AUDIO_INPUT_SCK, AUDIO_INPUT_WS, AUDIO_INPUT_DIN);
  // Initialize the I2S bus for audio output
  i2s_output_init(AUDIO_OUTPUT_BCLK, AUDIO_OUTPUT_LRC, AUDIO_OUTPUT_DOUT);

  // Create mutex for display buffer protection
  display_mutex = xSemaphoreCreateMutex();
  if (!display_mutex) {
    Serial.println("Warning: failed to create display mutex");
  }

  // Connect to WiFi (used for HTTP requests)
  wifi_connect();

  // Start VAD task (but in disabled state initially)
  xTaskCreate(vad_task, "vad_task", 4096, NULL, 1, &vad_task_handle_internal);
  vad_task_handle = NULL;  // Start with VAD disabled

  Serial.println("Serial commands: (t)est server, (i)p info\n");
}

// Main loop function that runs continuously
void loop() {
  // Apply any pending display requests from background tasks
  if (display_line1_pending) {
    if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
    char tmp[128];
    strncpy(tmp, display_line1_buf, sizeof(tmp));
    display_line1_pending = false;
    if (display_mutex) xSemaphoreGive(display_mutex);
    display.displayLine1(tmp);
  }
  if (display_line2_pending) {
    if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
    char tmp2[128];
    strncpy(tmp2, display_line2_buf, sizeof(tmp2));
    display_line2_pending = false;
    if (display_mutex) xSemaphoreGive(display_mutex);
    display.displayLine2(tmp2);
  }
  if (display_boot_show_pending) {
    if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
    char tmp3[128];
    strncpy(tmp3, display_boot_buf, sizeof(tmp3));
    display_boot_show_pending = false;
    if (display_mutex) xSemaphoreGive(display_mutex);
    display.showBootInstructions(tmp3);
  } else if (display_boot_hide_pending) {
    if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
    display_boot_hide_pending = false;
    if (display_mutex) xSemaphoreGive(display_mutex);
    display.hideBootInstructions();
  }
  if (display_clear_pending) {
    if (display_mutex) xSemaphoreTake(display_mutex, portMAX_DELAY);
    display_clear_pending = false;
    if (display_mutex) xSemaphoreGive(display_mutex);
    display.clearLines();
  }
  display.routine(); 
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
  http.addHeader("Connection", "keep-alive");

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
  http.addHeader("Connection", "keep-alive");

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

    if (contentType.indexOf("audio/") == 0 || contentType.indexOf("audio") >= 0) {
      // If a recorder is running, request it to stop and wait briefly
      if (is_recorder_task_running()) {
        Serial.println("Recorder active when starting TTS: requesting stop...");
        stop_recorder_task();
        unsigned long wait_start = millis();
        while (is_recorder_task_running() && (millis() - wait_start) < 2000) {
          vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        if (is_recorder_task_running()) {
          Serial.println("Warning: recorder did not stop before TTS start");
        }
      }
      // Mark player as active during TTS playback (prevents recorder from starting and pauses VAD)
      player_task_handle = vad_task_handle_internal;  // Use non-NULL sentinel value

      // Stream audio: parse WAV header when it arrives, configure I2S, then feed PCM chunks to I2S.
      WiFiClient *client = http.getStreamPtr();
      if (!client) {
        Serial.println("No stream pointer available for TTS audio.");
        player_task_handle = NULL;  // Clear player active state
        http.end();
        return;
      }

      const size_t CHUNK = 1024;
      uint8_t *stream_buf = (uint8_t *)malloc(CHUNK);
      uint8_t *convert_buf = NULL; // allocated on-demand for mono->stereo conversion
      if (!stream_buf) {
        Serial.println("Failed to allocate streaming buffer");
        http.end();
        return;
      }

      uint8_t header_tmp[1024];
      size_t header_pos = 0;
      bool header_parsed = false;
      uint32_t sample_rate = 32000;
      uint16_t channels = 2;
      uint16_t bits_per_sample = 32;

      unsigned long start_ms = millis();
      while (client->connected() || client->available()) {
        // Allow UI to request an immediate stop of playback (by clearing player handle)
        if (player_task_handle == NULL) {
          Serial.println("TTS playback aborted by user request");
          break;
        }
        if (client->available()) {
          int avail = client->available();
          int toread = (avail > (int)CHUNK) ? (int)CHUNK : avail;
          if (toread <= 0) {
            // nothing to read this iteration
            delay(2);
            continue;
          }
          int r = client->readBytes((char *)stream_buf, toread);
          if (r <= 0) break;

          size_t p = 0;
          if (!header_parsed) {
            // Accumulate header bytes (allow larger headers with extra fmt fields)
            size_t need = sizeof(header_tmp) - header_pos;
            size_t take = (r < (int)need) ? r : need;
            memcpy(header_tmp + header_pos, stream_buf, take);
            header_pos += take;
            p += take;

            // Try parsing when we have at least 44 bytes; continue collecting if parse fails
            if (header_pos >= 44) {
              // Robust WAV parser: search for 'RIFF'/'WAVE' and then iterate chunks to find 'fmt '
              bool ok = false;
              size_t off = 0;
              if (header_tmp[0] == 'R' && header_tmp[1] == 'I' && header_tmp[2] == 'F' && header_tmp[3] == 'F') {
                // iterate chunks
                off = 12;
                while (off + 8 <= header_pos) {
                  const char *cid = (const char *)(header_tmp + off);
                  uint32_t csize = (uint32_t)header_tmp[off+4] | ((uint32_t)header_tmp[off+5] << 8) | ((uint32_t)header_tmp[off+6] << 16) | ((uint32_t)header_tmp[off+7] << 24);
                  size_t chunk_data = off + 8;
                  if (chunk_data + csize > header_pos) break; // not enough bytes yet
                  if (cid[0]=='f' && cid[1]=='m' && cid[2]=='t' && cid[3]==' ') {
                    if (csize >= 16) {
                      channels = (uint16_t)header_tmp[chunk_data+2] | ((uint16_t)header_tmp[chunk_data+3] << 8);
                      sample_rate = (uint32_t)header_tmp[chunk_data+4] | ((uint32_t)header_tmp[chunk_data+5] << 8) | ((uint32_t)header_tmp[chunk_data+6] << 16) | ((uint32_t)header_tmp[chunk_data+7] << 24);
                      bits_per_sample = (uint16_t)header_tmp[chunk_data+14] | ((uint16_t)header_tmp[chunk_data+15] << 8);
                      ok = true;
                    }
                  }
                  off = chunk_data + csize;
                  if (csize & 1) off++;
                }
              }

              if (ok) {
                Serial.printf("Parsed WAV header: sr=%u ch=%u bps=%u\r\n", sample_rate, channels, bits_per_sample);
                if (!i2s_output_stream_begin(sample_rate, bits_per_sample, channels)) {
                  Serial.println("Failed to begin I2S streaming for TTS audio");
                  break;
                }
                header_parsed = true;
                // If there are remaining bytes in the accumulated header after the header offset,
                // locate where 'data' chunk starts to compute the correct PCM start point. As a
                // pragmatic fallback, write all remaining bytes after the whole header buffer.
                // If the current chunk contained PCM after the fmt chunk, write leftover bytes.
                if (header_pos > off) {
                  size_t leftover = header_pos - off;
                  // If the audio is mono but I2S is stereo by configuration, duplicate samples
                  if (channels == 1 && bits_per_sample > 0) {
                    size_t sample_bytes = (bits_per_sample + 7) / 8;
                    size_t samples = leftover / sample_bytes;
                    size_t out_bytes = samples * sample_bytes * 2;
                    if (!convert_buf) convert_buf = (uint8_t *)malloc(out_bytes > CHUNK*2 ? out_bytes : CHUNK*2);
                    uint8_t *outp = convert_buf;
                    uint8_t *inp = header_tmp + off;
                    for (size_t si = 0; si < samples; ++si) {
                      // copy sample
                      memcpy(outp, inp, sample_bytes);
                      outp += sample_bytes;
                      memcpy(outp, inp, sample_bytes);
                      outp += sample_bytes;
                      inp += sample_bytes;
                    }
                    i2s_output_stream_write(convert_buf, out_bytes);
                  } else {
                    i2s_output_stream_write(header_tmp + off, leftover);
                  }
                }
                // Also write any remaining bytes from this network read beyond what we consumed
                if (r - (int)p > 0) {
                  size_t have = r - (int)p;
                  if (channels == 1 && bits_per_sample > 0) {
                    size_t sample_bytes = (bits_per_sample + 7) / 8;
                    size_t samples = have / sample_bytes;
                    size_t out_bytes = samples * sample_bytes * 2;
                    if (!convert_buf) convert_buf = (uint8_t *)malloc(out_bytes > CHUNK*2 ? out_bytes : CHUNK*2);
                    uint8_t *outp = convert_buf;
                    uint8_t *inp = stream_buf + p;
                    for (size_t si = 0; si < samples; ++si) {
                      memcpy(outp, inp, sample_bytes); outp += sample_bytes;
                      memcpy(outp, inp, sample_bytes); outp += sample_bytes;
                      inp += sample_bytes;
                    }
                    i2s_output_stream_write(convert_buf, out_bytes);
                  } else {
                    i2s_output_stream_write(stream_buf + p, have);
                  }
                }
              } else {
                  // not enough header bytes to parse fmt/data yet; continue collecting
                  if (header_pos >= sizeof(header_tmp)) {
                    Serial.println("Header too large or malformed; aborting streaming playback.");
                    break;
                  }
                }
            }
          } else {
            // Header already parsed: write PCM directly
              if (channels == 1 && bits_per_sample > 0) {
                size_t sample_bytes = (bits_per_sample + 7) / 8;
                size_t samples = r / sample_bytes;
                size_t out_bytes = samples * sample_bytes * 2;
                if (!convert_buf) convert_buf = (uint8_t *)malloc(out_bytes > CHUNK*2 ? out_bytes : CHUNK*2);
                uint8_t *outp = convert_buf;
                uint8_t *inp = stream_buf;
                for (size_t si = 0; si < samples; ++si) {
                  memcpy(outp, inp, sample_bytes); outp += sample_bytes;
                  memcpy(outp, inp, sample_bytes); outp += sample_bytes;
                  inp += sample_bytes;
                }
                i2s_output_stream_write(convert_buf, out_bytes);
              } else {
                i2s_output_stream_write(stream_buf, r);
              }
          }
          start_ms = millis();
          // Check if user requested stop during processing
          if (player_task_handle == NULL) {
            Serial.println("TTS playback abort requested after chunk");
            break;
          }
        } else {
          // no data available right now; give CPU to other tasks
          delay(2);
          if (millis() - start_ms > 30000) {
            Serial.println("Timeout waiting for streaming audio data");
            break;
          }
        }
      }
      // Cleanup after streaming loop
      if (convert_buf) free(convert_buf);
      free(stream_buf);
      if (header_parsed) {
        // Give a small time for the output buffer to drain then end stream
        delay(20);
        i2s_output_stream_end();
      } else {
        Serial.println("No streaming audio played (header not parsed)");
      }
      // Clear player handle and re-enable VAD after TTS playback
      player_task_handle = NULL;
      // If VAD was auto-disabled to process this utterance, re-enable it now
      if (vad_task_handle == NULL) {
        vad_task_handle = vad_task_handle_internal;
        request_clear_lines();
        request_display_line1("Ready to listen.");
      }
    } else if (contentType.indexOf("application/json") >= 0) {
      String payload = http.getString();
      Serial.println("TTS returned JSON instead of audio: ");
      Serial.println(payload);
    } else {
      Serial.printf("Unexpected Content-Type from TTS: %s\r\n", contentType.c_str());
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

  if (!whisper_client.connected()) {
    Serial.printf("Connecting to %s:%d\r\n", host, port);
    if (!whisper_client.connect(host, port)) {
      Serial.println("Connection failed");
      return;
    }
  }

  // Send request headers
  whisper_client.print(String("POST ") + path + " HTTP/1.1\r\n");
  whisper_client.print(String("Host: ") + host + ":" + port + "\r\n");
  whisper_client.print("User-Agent: ESP32\r\n");
  whisper_client.print(String("Content-Type: multipart/form-data; boundary=") + boundary + "\r\n");
  whisper_client.print(String("Content-Length: ") + content_length + "\r\n");
  whisper_client.print("Connection: keep-alive\r\n\r\n");

  // Send multipart body: model part
  whisper_client.print(part_model);
  // Send file header
  whisper_client.print(part_file_header);

  // Send WAV header
  write_wav_header_to_client(whisper_client, length);

  // Send PCM data from PSRAM in chunks
  size_t sent = 0;
  const size_t CHUNK = 1024;
  while (sent < length) {
    size_t to_send = (length - sent) > CHUNK ? CHUNK : (length - sent);
    whisper_client.write(buffer + sent, to_send);
    sent += to_send;
    // optional small yield to avoid watchdog
    yield();
  }

  // Send ending boundary
  whisper_client.print(part_end);

  Serial.println("Request sent, waiting for response...");
  request_display_line2("Processing...");

  // Read response
  unsigned long timeout = millis() + 10000; // 10s timeout
  String response = "";
  while (whisper_client.connected() || whisper_client.available()) {
    while (whisper_client.available()) {
      char c = whisper_client.read();
      response += c;
    }
    if (millis() > timeout) break;
    delay(5);
  }
  // Do not stop the client to keep connection alive

  // Truncate large response for logging
  if (response.length() > 8192) response = response.substring(response.length() - 8192);

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
    // Show the transcribed text on the display (wrapped, no scrolling)
    request_display_line1(text.c_str());
    request_display_line2("Generating response...");
    // Send transcription to Ollama
    send_transcription_to_ollama(text);
  } else {
    // Clear the "Processing..." message on the display (request main-loop to handle LVGL)
    request_clear_lines();

    Serial.println("No transcription found in response.");
    request_display_line1("No speech detected. Ready to listen.");
    vTaskDelay(2000 / portTICK_PERIOD_MS);  // Wait 2 seconds
  }

    // Re-enable VAD after processing (if it was auto-disabled)
    if (vad_task_handle == NULL) {
      vad_task_handle = vad_task_handle_internal;
      request_display_line1("Ready to listen.");
    }
}

// Function to handle button events
void handle_button_events() {
  // Get the current state of the button
  int button_state = button.get_button_state();
  // Get the key value associated with the button press
  int button_key_value = button.get_button_key_value();
  // Switch case based on the button key value
  // Toggle VAD only on the debounced PRESSED edge (rising edge)
  if (button_state == Button::KEY_STATE_PRESSED && last_button_state_for_toggle != Button::KEY_STATE_PRESSED) {
    if (vad_task_handle != NULL) {
      // Disable VAD by clearing the handle
      vad_task_handle = NULL;

      request_clear_lines();
      request_display_line2("Stopping...");

      // Stop any active playback
      if (player_task_handle != NULL) {
        Serial.println("VAD disabled: stopping player");
        stop_player_task();
        // End I2S streaming so audio stops quickly
        i2s_output_stream_end();
      }

      // Stop any active recording
      if (recorder_task_handle != NULL) {
        Serial.println("VAD disabled: stopping recorder");
        stop_recorder_task();
      }

      // Show boot instructions again
      request_showBootInstructions("Press button to start a conversation.\nCurrently not listening.");
      Serial.println("VAD disabled via button");
    } else {
      // Enable VAD by setting the handle
      vad_task_handle = vad_task_handle_internal;
      request_hideBootInstructions();
      request_clear_lines();
      request_display_line1("Button pressed: Listening enabled.");
      Serial.println("VAD enabled via button");
    }
  }
  // Update last button state for edge detection
  last_button_state_for_toggle = button_state;
}

/* Start recording task */
void start_recorder_task(void) {
  // Do not start recorder while player is active
  if (player_task_handle != NULL) {
    Serial.println("Recorder start suppressed: player active");
    return;
  }
  // Check if the recorder task is not already running
  if (recorder_task_handle == NULL) {
    // Create a new task for recording sound, store its handle
    xTaskCreate(loop_task_sound_recorder, "loop_task_sound_recorder", 4096, NULL, 1, &recorder_task_handle);
  }
}

/* Stop recording task */
void stop_recorder_task(void) {
  // Request the recorder task to stop via its task handle (graceful stop)
  if (recorder_task_handle != NULL) {
    Serial.println("Signaling loop_task_sound_recorder to stop...");
    request_display_line1("Please wait...");
    // Clear the handle to signal stop and send notification
    TaskHandle_t temp = recorder_task_handle;
    recorder_task_handle = NULL;
    xTaskNotifyGive(temp);
  } else {
    Serial.println("Recorder task not running");
  }
}

/* Check if recording task is active */
int is_recorder_task_running(void) {
  // Return the status based on handle
  return (recorder_task_handle != NULL) ? 1 : 0;
}

/* Main recording task loop */
void loop_task_sound_recorder(void *pvParameters) {
  // Print a message indicating the start of the recording task
  Serial.println("loop_task_sound_recorder start...");
  // Initialize the total size of recorded data
  int total_size = 0;
  bool stop_requested = false;
  // Allocate memory in PSRAM for storing audio data
  if(wav_buffer!=NULL && player_task_handle == NULL)
  {
    // Free the allocated memory in PSRAM only if player is not using it
    heap_caps_free(wav_buffer);
    wav_buffer = NULL;
  }
  wav_buffer = (uint8_t *)heap_caps_malloc(MOLLOC_SIZE, MALLOC_CAP_SPIRAM);

  // No filesystem: record directly into PSRAM buffer

  // Loop until a stop notification is received or handle is cleared
  while (!stop_requested && recorder_task_handle != NULL) {
    request_display_line1("Listening...");
    // Get the available IIS data size
    int iis_buffer_size = audio_input_get_iis_data_available();
    // Loop while there is IIS data available
    while (iis_buffer_size > 0) {
      // Check for a stop notification (non-blocking) or if handle was cleared
      if (ulTaskNotifyTake(pdTRUE, 0) > 0 || recorder_task_handle == NULL) {
        Serial.println("Stop requested for recorder task");
        request_display_line1("Stopped Listening - Task Stopped");
        stop_requested = true;
        break;
      }
      // Check if the buffer is full
      if ((total_size + 512) >= MOLLOC_SIZE) {
        // Stop the recorder task if the buffer is full
        Serial.println("Buffer full, stopping recorder task");
        request_display_line1("Stopped Listening - Buffer Full");
        stop_requested = true;
        break;
      }

      // Read IIS data into the buffer
      int real_size = audio_input_read_iis_data((char*)wav_buffer + total_size, 512);
      // Update the total size of recorded data
      total_size += real_size;
      // Decrease the available IIS data size
      iis_buffer_size -= real_size;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);  // Small delay to prevent busy loop
  }

  last_recorded_size = total_size;
  Serial.printf("Recorded bytes in PSRAM: %u\r\n", (unsigned)last_recorded_size);

  Serial.printf("write wav size:%d\r\n", total_size);
  // Stream the recorded WAV (header + PSRAM buffer) to the transcription server
  post_wav_stream_psram(STT_MODEL, wav_buffer, total_size);
  // Print a message indicating the end of the recording task
  Serial.println("loop_task_sound_recorder stop...");
  // Clear handle and delete the current task
  recorder_task_handle = NULL;
  vTaskDelete(NULL);
}

/* Start player task */
void start_player_task(void) {
  // Check if the player task is not already running
  if (player_task_handle == NULL) {
    xTaskCreate(loop_task_play_handle, "loop_task_play_handle", 4096, NULL, 1, &player_task_handle);
  }
}

/* Stop player task */
void stop_player_task(void) {
  // Request player task to stop by notifying it
  if (player_task_handle != NULL) {
    Serial.println("Signaling loop_task_play_handle to stop...");
    // Clear the handle to signal stop and send notification
    TaskHandle_t temp = player_task_handle;
    player_task_handle = NULL;
    xTaskNotifyGive(temp);
  } else {
    Serial.println("Player task not running");
  }
}

/* Check if player task is active */
int is_player_task_running(void) {
  // Return the status based on handle
  return (player_task_handle != NULL) ? 1 : 0;
}

/* Main player task loop */
void loop_task_play_handle(void *pvParameters) {
  // Print a message indicating the start of the player task
  Serial.println("loop_task_play_handle start...");
  bool stop_requested = false;
  // Loop while the player task is running and handle is not NULL
  while (!stop_requested && player_task_handle != NULL) {
      // Check for a stop notification (non-blocking) or if handle was cleared
      if (ulTaskNotifyTake(pdTRUE, 0) > 0 || player_task_handle == NULL) {
        request_display_line1("Stopped Responding - Task Stopped");
        stop_requested = true;
        break;
      }
      // Play the last in-memory recording (PSRAM)
      if (wav_buffer != NULL && last_recorded_size > 0) {
        Serial.printf("Playing in-memory recording, size=%u\r\n", (unsigned)last_recorded_size);
        i2s_output_wav(wav_buffer, last_recorded_size);
      } else {
        Serial.println("No in-memory recording available to play.");
      }
      // After playback, stop
      request_display_line1("Stopped Responding - Task Finished");
      stop_requested = true;
  }
  // Print a message indicating the end of the player task
  Serial.println("loop_task_play_handle stop...");
  // Clear handle and delete the current task
  player_task_handle = NULL;
  vTaskDelete(NULL);
}

/* VAD task for automatic voice activity detection */
void vad_task(void *pvParameters) {
  char buffer[1024];  // 128 stereo samples * 8 bytes = 1024 bytes

  while (true) {
    // Skip processing if VAD is disabled (handle is NULL) or button is pressed or player is active
    if (vad_task_handle == NULL || button.get_button_state() == Button::KEY_STATE_PRESSED || player_task_handle != NULL) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }

    int available = audio_input_get_iis_data_available();
    if (available >= 1024) {
      int read_size = audio_input_read_iis_data(buffer, 1024);
      vad_samples = 0;
      if (read_size > 0) {
        // Process interleaved 32-bit stereo samples. Compute number of stereo pairs
        // from the number of bytes read to avoid hard-coded sizes.
        int32_t *samples = (int32_t *)buffer;
        int num_samples = read_size / (sizeof(int32_t) * 2);  // stereo pairs
        if (num_samples <= 0) continue;
        // First pass: find maximum absolute sample to choose a safe downshift so
        // the values fit in a 16-bit range without blindly shifting by 16.
        uint32_t max_abs = 0;
        for (int i = 0; i < num_samples; i++) {
          int32_t left = samples[i * 2];
          uint32_t abs32 = (left < 0) ? (uint32_t)(-left) : (uint32_t)left;
          if (abs32 > max_abs) max_abs = abs32;
        }

        int shift = 0;
        while (shift < 31 && (max_abs >> shift) > 32767) shift++;

        // Compute and remove DC bias (mean) before measuring absolute energy
        long long sum_signed = 0;
        for (int i = 0; i < num_samples; i++) {
          int32_t left = samples[i * 2];
          sum_signed += (long long)left;
        }
        int32_t mean_signed = (int32_t)(sum_signed / num_samples);

        long long sum = 0;
        for (int i = 0; i < num_samples; i++) {
          int32_t left = samples[i * 2];
          int32_t centered = left - mean_signed;
          uint32_t abs32 = (centered < 0) ? (uint32_t)(-centered) : (uint32_t)centered;
          uint32_t scaled = (shift > 0) ? (abs32 >> shift) : abs32;
          sum += (long long)scaled;
        }
        int avg = (int)(sum / num_samples);

        if (avg > VAD_THRESHOLD) {
          vad_samples++;
          Serial.printf("Recording... Samples: %d VAD Low Energy: %d VAD avg: %d\n", vad_samples, vad_low_energy_count, avg);
          vad_low_energy_count = 0;
          if (recorder_task_handle == NULL) {
            Serial.println("VAD: Start recording");
            request_display_line1("Detected sound...");
            start_recorder_task();
          }
        } else {
          vad_low_energy_count++;
          Serial.printf("Recording... Samples: %d VAD Low Energy: %d VAD avg: %d\n", vad_samples, vad_low_energy_count, avg);
          if (recorder_task_handle != NULL && abs(vad_samples - vad_low_energy_count) >= VAD_LOW_COUNT) {
            Serial.println("VAD: Stop recording");
            stop_recorder_task();
            vad_task_handle = NULL;  // Disable VAD while processing the utterance
            vad_low_energy_count = 0;
          }
        }
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);  // ~50ms delay for processing
  }
}
