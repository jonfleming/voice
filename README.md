**Voice Assistant**

A small voice assistant that integrates Wyoming Faster Whisper (STT), Ollama (LLM), and Wyoming Piper (TTS). The main script is `voice.py`.

**Prerequisites**
- **OS:** Linux (instructions below use Debian/Ubuntu examples).
- **System packages:** `ffmpeg` and PortAudio development headers (needed for `pyaudio`).
- **Python:** 3.8+ recommended.

**Install System Dependencies (Debian/Ubuntu)**
- Option A — install distro packages (easiest):

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip python3-pyaudio ffmpeg
sudo apt install -y portaudio19-dev
```

- Option B — if you prefer to build `pyaudio` from pip:

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip build-essential portaudio19-dev libsndfile1 ffmpeg
```

**Python Environment and Python Packages**
- Create and activate a virtual environment, upgrade pip, and install required Python packages.

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install requests pyaudio
```

Note: If you installed `python3-pyaudio` via `apt` (Option A), you may not need to `pip install pyaudio`.

**Configuration**
- Open `voice.py` and edit the top configuration variables as needed:

- **`SERVER_IP`**: IP address of your server running Whisper/Piper/Ollama
- **`WHISPER_PORT`**, **`PIPER_PORT`**, **`OLLAMA_PORT`**: ports for each service
- **`OLLAMA_MODEL`**, **`STT_MODEL`**, **`TTS_MODEL`**, **`TTS_VOICE`**: model/voice choices used by the script

These values are near the top of `voice.py`.

**Run the Assistant**

Activate the environment (if not already) and run:

```bash
source .venv/bin/activate
python voice.py
```

The assistant will record from the default microphone, send audio to the STT server, query Ollama, then send the response to the TTS server and play audio.

**Troubleshooting**
- Error: `TTS error: file does not start with RIFF id` — This means the script received non-WAV bytes (e.g., MP3/OGG or a JSON error). The script includes an ffmpeg-based fallback to convert non-WAV audio to WAV before playback. Ensure `ffmpeg` is installed and on your `PATH`.

- If the TTS endpoint returns JSON (an error message), you will see the JSON printed in the console. Inspect that output to debug server-side problems (model not found, invalid parameters, etc.).

- Microphone capture or `pyaudio` issues on Linux: verify your user has audio device permissions and that ALSA/PulseAudio are configured. Try `arecord -l` or `aplay -l` to list devices.

**Debugging TTS Responses Manually**
You can call the Piper/TTS endpoint with `curl` to inspect the raw response and its type:

```bash
curl -v -X POST "http://<SERVER_IP>:<PIPER_PORT>/v1/audio/speech" \
  -H "Content-Type: application/json" \
  -d '{"model":"speaches-ai/Kokoro-82M-v1.0-ONNX","voice":"af_heart","input":"hello"}' \
  --output out.bin

file out.bin
hexdump -C out.bin | head
```

`file out.bin` will report whether the server returned `RIFF (WAV)`, `MP3`, `OGG`, or some other format.

**Optional**
- If you want, I can add a `requirements.txt` or automatically save raw TTS responses for debugging. Ask me to add those and I will.

**Contact**
- If you run into issues, paste console output and I can help debug further.

**License**
- Use and modify as you like; no explicit license provided here.
