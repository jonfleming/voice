import pyaudio

audio = pyaudio.PyAudio()

# Check which host APIs are available
print("Available Host APIs:")
for i in range(audio.get_host_api_count()):
    info = audio.get_host_api_info_by_index(i)
    print(f"  {i}: {info['name']}")

# List devices for each host API
for api_idx in range(audio.get_host_api_count()):
    api_info = audio.get_host_api_info_by_index(api_idx)
    print(f"\nDevices for {api_info['name']}:")
    for dev_idx in range(api_info['deviceCount']):
        try:
            dev_info = audio.get_device_info_by_host_api_device_index(api_idx, dev_idx)
            print(f"  {dev_idx}: {dev_info['name']} (out: {dev_info['maxOutputChannels']})")
        except:
            pass
