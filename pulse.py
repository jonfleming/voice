import pyaudio
p = pyaudio.PyAudio()
print("Host APIs:")
for i in range(p.get_host_api_count()):
    info = p.get_host_api_info_by_index(i)
    print(f"Index {i}: {info['name']} (type {info['type']})")

print("\nOutput Devices (ALSA usually 0):")
for i in range(p.get_device_count()):
    dev = p.get_device_info_by_index(i)
    if dev['maxOutputChannels'] > 0:
        print(f"Device {i}: {dev['name']} (host {dev['hostApi']}, ch {dev['maxOutputChannels']}, rate {dev['defaultSampleRate']})")
p.terminate()
