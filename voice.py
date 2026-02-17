#!/usr/bin/env python3
"""
Voice Assistant for Raspberry Pi 5
Integrates Wyoming Faster Whisper, Ollama, and Wyoming Piper
"""

import wave
import io
import pyaudio
import requests
import subprocess
import shutil
import time

# Configuration - Update these with your server details
SERVER_IP = "192.168.0.108"  # Replace with your server IP
WHISPER_PORT = 8000  # Default Wyoming Faster Whisper port
PIPER_PORT = 8000    # Default Wyoming Piper port
OLLAMA_PORT = 11434   # Default Ollama port
OLLAMA_MODEL = "llama3.2"  # Change to your preferred model
STT_MODEL = "Systran/faster-distil-whisper-small.en"  # Change to your preferred STT model
TTS_MODEL = "speaches-ai/Kokoro-82M-v1.0-ONNX"
TTS_VOICE = "af_heart"

# Audio settings
CHUNK = 1024
FORMAT = pyaudio.paInt16
CHANNELS = 1
RATE = 44100
RECORD_SECONDS = 5  # Maximum recording time
SILENCE_THRESHOLD = 500  # Adjust based on your environment
SILENCE_DURATION = 2  # Seconds of silence to stop recording

class VoiceAssistant:
    def __init__(self):
        self.audio = pyaudio.PyAudio()
        
    def flush_input(self):
        """Flush any existing audio input to avoid processing old data"""
        stream = self.audio.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=RATE,
            input=True,
            frames_per_buffer=CHUNK
        )
        data = stream.read(CHUNK, exception_on_overflow=False)
        print(f"Flushed {len(data)} bytes of audio input {data[:10]}...")
        stream.stop_stream()
        stream.close()
        time.sleep(0.1)
        
    def record_audio(self):
        """Record audio from microphone until silence is detected"""
        print("Listening... (speak now)")
        
        stream = self.audio.open(
            format=FORMAT,
            channels=CHANNELS,
            rate=RATE,
            input=True,
            frames_per_buffer=CHUNK
        )
        
        frames = []
        silent_chunks = 0
        silence_limit = int(SILENCE_DURATION * RATE / CHUNK)
        
        for i in range(0, int(RATE / CHUNK * RECORD_SECONDS)):
            data = stream.read(CHUNK)
            print(f"Read {len(data)} bytes of audio input {data[:10]}...")
            frames.append(data)
            
            # Check for silence
            audio_data = sum(abs(int.from_bytes(data[i:i+2], 'little', signed=True)) 
                           for i in range(0, len(data), 2)) / (len(data) / 2)
            
            if audio_data < SILENCE_THRESHOLD:
                silent_chunks += 1
            else:
                silent_chunks = 0
            
            if silent_chunks > silence_limit:
                break
        
        stream.stop_stream()
        stream.close()
        print("Recording complete")
        
        # Convert to WAV format
        wav_buffer = io.BytesIO()
        with wave.open(wav_buffer, 'wb') as wf:
            wf.setnchannels(CHANNELS)
            wf.setsampwidth(self.audio.get_sample_size(FORMAT))
            wf.setframerate(RATE)
            wf.writeframes(b''.join(frames))
        
        return wav_buffer.getvalue()        

    
    def transcribe_audio(self, audio_data):
        """Send audio to Wyoming Faster Whisper for transcription"""
        print("Transcribing...")
        
        url = f"http://{SERVER_IP}:{WHISPER_PORT}/v1/audio/transcriptions"
        
        files = {
            'file': ('audio.wav', audio_data, 'audio/wav')
        }
        data = {
            'model': STT_MODEL
        }
        
        try:
            response = requests.post(url, files=files, data=data, timeout=30)
            response.raise_for_status()
            result = response.json()
            text = result.get('text', '').strip()
            print(f"Transcribed: {text}")
            return text
        except Exception as e:
            print(f"Transcription error: {e}")
            return None
    
    def get_ollama_response(self, prompt):
        """Send prompt to Ollama and get response"""
        print("Getting AI response...")
        
        url = f"http://{SERVER_IP}:{OLLAMA_PORT}/api/generate"
        
        payload = {
            'model': OLLAMA_MODEL,
            'prompt': prompt,
            'stream': False
        }
        
        try:
            response = requests.post(url, json=payload, timeout=60)
            response.raise_for_status()
            result = response.json()
            ai_response = result.get('response', '').strip()
            print(f"AI Response: {ai_response}")
            return ai_response
        except Exception as e:
            print(f"Ollama error: {e}")
            return None
    
    def text_to_speech(self, text):
        """Send text to Wyoming Piper for TTS and play audio"""
        print("Converting to speech...")
        
        url = f"http://{SERVER_IP}:{PIPER_PORT}/v1/audio/speech"
        
        payload = {
            'model': TTS_MODEL,
            'voice': TTS_VOICE,
            'input': text,
        }
        
        try:
            response = requests.post(url, json=payload, timeout=30)
            response.raise_for_status()

            # Inspect content type
            content_type = response.headers.get('Content-Type', '')

            if content_type.startswith('application/json'):
                # Likely an error payload or metadata
                try:
                    payload_json = response.json()
                except Exception:
                    payload_json = {'raw': response.text}
                print(f"TTS returned JSON instead of audio: {payload_json}")
                return

            # Play the audio (may not be WAV) - play_audio will attempt conversion
            audio_data = response.content
            self.play_audio(audio_data)

        except Exception as e:
            print(f"TTS error: {e}")
    
    def play_audio(self, audio_data):
        """Save Piper TTS response to a .wav file and play it using a subprocess. Allow Ctrl-C to interrupt playback only."""
        try:
            # If the data isn't a WAV (RIFF) try to convert it via ffmpeg
            if not audio_data.startswith(b'RIFF'):
                try:
                    audio_data = self._convert_to_wav_ffmpeg(audio_data)
                except Exception as e:
                    print(f"TTS conversion error: {e}")
                    return

            import tempfile
            with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as tmp_wav:
                tmp_wav.write(audio_data)
                tmp_wav_path = tmp_wav.name

            # Play using a subprocess (paplay or ffplay), allow interruption
            player_cmd = None
            if shutil.which('paplay'):
                player_cmd = ['paplay', tmp_wav_path]
            elif shutil.which('ffplay'):
                player_cmd = ['ffplay', '-nodisp', '-autoexit', tmp_wav_path]
            else:
                print("No suitable audio player found (paplay or ffplay required)")
                import os
                os.remove(tmp_wav_path)
                return

            proc = None
            try:
                self._mute_mic()
                print("Playing audio...")
                proc = subprocess.Popen(player_cmd)
                while proc.poll() is None:
                    try:
                        time.sleep(0.1)
                    except KeyboardInterrupt:
                        print("Playback interrupted by Ctrl-C")
                        proc.terminate()
                        proc.wait()
                        break
            except Exception as e:
                print(f"Audio playback error: {e}")
            finally:
                import os
                os.remove(tmp_wav_path)

            print("Playback complete")
        finally:
            print("Waiting for audio to settle...")
            time.sleep(1.0)
            self._unmute_mic()
            print("\nflushing input...")
            self.flush_input()  


    def _mute_mic(self):
        """Mute the microphone to prevent feedback during playback"""
        try:
            # Try PulseAudio first (common on modern Linux)
            subprocess.run(['pactl', 'set-source-mute', '@DEFAULT_SOURCE@', '1'], 
                         check=True, capture_output=True)
            print("Microphone muted (PulseAudio)")
        except subprocess.CalledProcessError:
            try:
                # Fallback to ALSA
                subprocess.run(['amixer', 'set', 'Capture', 'nocap'], 
                             check=True, capture_output=True)
                print("Microphone muted (ALSA)")
            except subprocess.CalledProcessError:
                print("Warning: Could not mute microphone")
    
    def _unmute_mic(self):
        """Unmute the microphone after playback"""
        try:
            # Try PulseAudio first
            subprocess.run(['pactl', 'set-source-mute', '@DEFAULT_SOURCE@', '0'], 
                         check=True, capture_output=True)
            print("Microphone unmuted (PulseAudio)")
        except subprocess.CalledProcessError:
            try:
                # Fallback to ALSA
                subprocess.run(['amixer', 'set', 'Capture', 'cap'], 
                             check=True, capture_output=True)
                print("Microphone unmuted (ALSA)")
            except subprocess.CalledProcessError:
                print("Warning: Could not unmute microphone")

    def _convert_to_wav_ffmpeg(self, audio_bytes: bytes) -> bytes:
        """Convert arbitrary audio bytes to WAV using ffmpeg (requires ffmpeg on PATH).

        Returns WAV bytes (RIFF) or raises RuntimeError on failure.
        """
        if not shutil.which('ffmpeg'):
            raise RuntimeError('ffmpeg not found on PATH; install ffmpeg to convert audio')

        cmd = [
            'ffmpeg',
            '-hide_banner',
            '-loglevel', 'error',
            '-i', 'pipe:0',
            '-f', 'wav',
            '-ar', str(RATE),
            '-ac', str(CHANNELS),
            'pipe:1'
        ]

        proc = subprocess.run(cmd, input=audio_bytes, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if proc.returncode != 0:
            raise RuntimeError(f"ffmpeg conversion failed: {proc.stderr.decode(errors='replace')}")

        if not proc.stdout.startswith(b'RIFF'):
            raise RuntimeError('ffmpeg did not produce a valid WAV (missing RIFF header)')

        return proc.stdout
    
    def run(self):
        """Main loop"""
        print("Voice Assistant Started")
        print(f"Server: {SERVER_IP}")
        print(f"Model: {OLLAMA_MODEL}")
        print("\nPress Ctrl+C to exit\n")
        
        try:
            while True:
                # Record audio
                audio_data = self.record_audio()
                
                # Transcribe
                text = self.transcribe_audio(audio_data)
                if not text:
                    print("No speech detected, trying again...\n")
                    continue
                
                # Get AI response
                response = self.get_ollama_response(text)
                if not response:
                    print("Failed to get response, trying again...\n")
                    continue
                
                # Convert to speech and play
                self.text_to_speech(response)                
                print("\n" + "="*50 + "\n")
                
        except KeyboardInterrupt:
            print("\nShutting down...")
        finally:
            self.audio.terminate()

if __name__ == "__main__":
    assistant = VoiceAssistant()
    assistant.run()
