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
import select
import sys
import time
from pvspeaker import PvSpeaker

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
RATE = 16000
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
        data = stream.read(CHUNK)
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
            frames.append(data)
            
            # Check for silence
            audio_data = sum(abs(int.from_bytes(data[i:i+2], 'little', signed=True)) 
                           for i in range(0, len(data), 2)) / (len(data) / 2)

            # Clear the current line before printing status
            print("\033[K", end="")
            print(f"Audio level: {audio_data:.2f} silent chunks: {silent_chunks}", end='\r')
            
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
        """Play audio through the speaker using PvSpeaker (Picovoice)"""
        print("Playing audio with PvSpeaker...")
        self._mute_mic()
        try:
            # If the data isn't a WAV (RIFF) try to convert it via ffmpeg
            if not audio_data.startswith(b'RIFF'):
                try:
                    audio_data = self._convert_to_wav_ffmpeg(audio_data)
                except Exception as e:
                    print(f"TTS conversion error: {e}")
                    return

            with wave.open(io.BytesIO(audio_data), 'rb') as wf:
                sample_rate = wf.getframerate()
                bits_per_sample = wf.getsampwidth() * 8
                channels = wf.getnchannels()
                n_frames = wf.getnframes()
                print(f"Audio format: {bits_per_sample} bits, {sample_rate} Hz, {channels} channels, {n_frames} frames")

                if channels != 1:
                    print("PvSpeaker currently only supports mono (1 channel) audio.")
                    return

                speaker = PvSpeaker(
                    sample_rate=sample_rate,
                    bits_per_sample=bits_per_sample
                )
                speaker.start()
                # Read all PCM data at once for correct buffer handling
                pcm_data = wf.readframes(n_frames)
                total_written = 0
                frame_size = wf.getsampwidth()
                # Write in chunks, as in pv.py
                while total_written < len(pcm_data):
                    written = speaker.write(pcm_data[total_written:total_written+CHUNK*frame_size])
                    total_written += written
                    if total_written < len(pcm_data):
                        time.sleep(0.01)
                speaker.flush()
                speaker.stop()
            print("Playback complete")
        finally:
            print("Waiting for audio to settle...")
            time.sleep(1.0)
            self._unmute_mic()
    
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
                self.flush_input()  
                
                print("\n" + "="*50 + "\n")
                
        except KeyboardInterrupt:
            print("\nShutting down...")
        finally:
            self.audio.terminate()

if __name__ == "__main__":
    assistant = VoiceAssistant()
    assistant.run()