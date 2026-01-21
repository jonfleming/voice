## Task Loops
- vad_task: Enabled at start.  Never disabled.
- loop_task_sound_recorder: started by `start_recorder_task()`. Stopped by `stop_recorder_task()` or stop_requested.
    Exits when: button press, buffer full, vad_enabled==falss
    When stopped, triggers `post_wav_stream_psram()`
        post_wav_stream_psram: sends whisper transcription to 
- loop_task_play_handle: 

## Flags
- vad_recording: when true, VAD will be active and may start/top recordings automatically
- vad_enabled: when true, VAD was auto-disabled (after recording) ***
- vad_auto_disabled: 

- tts_playing: to disable VAD when playing response
- tts_stop_requested: request to stop TTS playback
- playback_active:


