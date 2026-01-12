
#include "driver_audio_output.h"
#include "Audio.h"
#include <ESP_I2S.h>

Audio audio;
I2SClass i2s_output; 

bool i2s_output_init(int bclk, int lrc, int dout) {
  i2s_output.setPins(bclk, lrc, dout);
  if (!i2s_output.begin(I2S_MODE_STD, 32000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("Failed to initialize I2S output bus!");
    return false;
  }
  return true;
}

void i2s_output_wav(uint8_t *data, size_t len)
{
  // Inspect WAV header (if present) and reconfigure I2S to match sample rate / bit depth / channels
  // Use a robust chunk-based parser to handle non-standard fmt chunk sizes.
  if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' && data[8] == 'W' && data[9] == 'A' && data[10] == 'V' && data[11] == 'E') {
    uint32_t sample_rate = 32000;
    uint16_t channels = 2;
    uint16_t bits_per_sample = 32;

    // Parse chunks starting at offset 12
    size_t offset = 12;
    while (offset + 8 <= len) {
      // read chunk id and size
      const char *cid = (const char *)(data + offset);
      uint32_t csize = (uint32_t)data[offset+4] | ((uint32_t)data[offset+5] << 8) | ((uint32_t)data[offset+6] << 16) | ((uint32_t)data[offset+7] << 24);
      size_t chunk_data = offset + 8;
      if (chunk_data + csize > len) break; // not enough data available

      if (cid[0]=='f' && cid[1]=='m' && cid[2]=='t' && cid[3]==' ') {
        // fmt chunk: parse common fields if present
        if (csize >= 16) {
          channels = (uint16_t)data[chunk_data+2] | ((uint16_t)data[chunk_data+3] << 8);
          sample_rate = (uint32_t)data[chunk_data+4] | ((uint32_t)data[chunk_data+5] << 8) | ((uint32_t)data[chunk_data+6] << 16) | ((uint32_t)data[chunk_data+7] << 24);
          bits_per_sample = (uint16_t)data[chunk_data+14] | ((uint16_t)data[chunk_data+15] << 8);
        }
      }

      offset = chunk_data + csize;
      // chunk sizes are word-aligned to even bytes
      if (csize & 1) offset++;
    }

    Serial.printf("WAV header (parsed): sample_rate=%u, channels=%u, bits_per_sample=%u\r\n", sample_rate, channels, bits_per_sample);

    // Choose data bit width enum
    i2s_data_bit_width_t data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    if (bits_per_sample <= 16) data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;

    // Choose slot mode (mono/stereo)
    i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_STEREO;
#ifdef I2S_SLOT_MODE_MONO
    if (channels == 1) slot_mode = I2S_SLOT_MODE_MONO;
#else
    (void)channels; // silence unused variable when mono constant not defined
#endif

    // Reinitialize I2S with WAV parameters. End first to allow reconfiguration.
    i2s_output.end();
    if (!i2s_output.begin(I2S_MODE_STD, sample_rate, data_bit_width, slot_mode, I2S_STD_SLOT_BOTH)) {
      Serial.println("Failed to reinitialize I2S output with WAV parameters, falling back to default.");
      // attempt to reinit with default settings
      i2s_output.begin(I2S_MODE_STD, 32000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
    }
  }

  i2s_output.playWAV(data, len);
}

// Begin streaming playback: reconfigure I2S for the incoming WAV PCM parameters.
bool i2s_output_stream_begin(uint32_t sample_rate, uint16_t bits_per_sample, uint16_t channels) {
  // End any previous I2S to allow reconfiguration
  i2s_output.end();

  i2s_data_bit_width_t data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
  if (bits_per_sample <= 16) data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;

  i2s_slot_mode_t slot_mode = I2S_SLOT_MODE_STEREO;
#ifdef I2S_SLOT_MODE_MONO
  if (channels == 1) slot_mode = I2S_SLOT_MODE_MONO;
#endif

  if (!i2s_output.begin(I2S_MODE_STD, sample_rate, data_bit_width, slot_mode, I2S_STD_SLOT_BOTH)) {
    Serial.println("Failed to initialize I2S for streaming output");
    return false;
  }
  return true;
}

// Write PCM bytes to I2S output. Returns number of bytes written (best-effort).
size_t i2s_output_stream_write(const uint8_t *data, size_t len) {
  // Use I2SClass write method if available; this should stream PCM directly out.
  // The API below is a common signature for ESP_I2S variants; if your board
  // library uses a different name, adjust accordingly.
  size_t written = 0;
  if (len == 0) return 0;
  // `I2SClass::write` expects a non-const `uint8_t*` buffer. Cast away const
  // here because the I2S write will not modify the provided buffer.
  written = (size_t)i2s_output.write((uint8_t *)data, (size_t)len);
  return written;
}

void i2s_output_stream_end(void) {
  // Gracefully stop I2S streaming; don't deinit completely so caller can
  // re-use playback functions.
  i2s_output.end();
}

void i2s_output_deinit(void)
{ 
    i2s_output.end(); 
}

//Initialize the audio interface
int audio_output_init(int bclk, int lrc, int dout) {
  i2s_output_init(bclk, lrc, dout);
  i2s_output_deinit();
  return audio.setPinout(bclk, lrc, dout);
}

//Set the volume: 0-21
void audio_output_set_volume(int volume) {
  audio.setVolume(volume);
}

//Query volume
int audio_read_output_volume(void) {
  return audio.getVolume();
}

//Pause/play the music
void audio_output_pause_resume(void) {
  audio.pauseResume();
}

//Stop the music
void audio_output_stop(void) {
  audio.stopSong();
}

//Whether the music is running
bool audio_output_is_running(void) {
  return audio.isRunning();
}

//Gets how long the music player has been playing
long audio_get_total_output_playing_time(void) {
  return (long)audio.getTotalPlayingTime() / 1000;
}

//Obtain the playing time of the music file
long audio_output_get_file_duration(void) {
  return (long)audio.getAudioFileDuration();
}

//Set play position
bool audio_output_set_play_position(int second) {
  return audio.setAudioPlayPosition((uint16_t)second);
}

//Gets the current playing time of the music
long audio_read_output_play_position(void) {
  return audio.getAudioCurrentTime();
}

//Non-blocking music execution function
void audio_output_loop(void) {
  audio.loop();
}

// optional
void audio_info(const char *info) {
  Serial.print("info        ");
  Serial.println(info);
}

void audio_eof_mp3(const char *info) {  
  Serial.print("eof_mp3     ");
  Serial.println(info);
}
