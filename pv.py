from pvspeaker import PvSpeaker
import time

# --- Configuration: Adjust to match your PCM audio source format ---
SAMPLE_RATE = 48000  # e.g., 44100, 16000
BITS_PER_SAMPLE = 16 # e.g., 8, 16
# ----------------------------------------------------------------

# 1. Initialize PvSpeaker
speaker = PvSpeaker(
    sample_rate=SAMPLE_RATE,
    bits_per_sample=BITS_PER_SAMPLE
)

try:
    # 2. Start the audio output device
    speaker.start()

    # --- Replace this with your actual PCM data source ---
    # Example: Create a dummy buffer of PCM data for demonstration
    import array
    import math
    duration_secs = 2
    num_samples = int(SAMPLE_RATE * duration_secs)
    # Generate a simple sine wave (PCM data)
    pcm_data = array.array('h', [int(32767 * math.sin(2 * math.pi * 440 * i / SAMPLE_RATE)) for i in range(num_samples)])
    # --------------------------------------------------

    # 3. Stream PCM audio data
    total_written = 0
    while total_written < len(pcm_data):
        written = speaker.write(pcm_data[total_written:])
        total_written += written
        # Wait if the buffer is full
        if total_written < len(pcm_data):
            time.sleep(0.01)
    
    # 4. Flush the buffer to ensure all audio plays
    speaker.flush()

finally:
    # 5. Stop playback and release resources
    speaker.stop()
    speaker.delete()
