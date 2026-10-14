import wave
import math
import struct

def create_sine_wave(freq, duration, samplerate=44100, filename="sine.wav"):
    nframes = int(samplerate * duration)
    with wave.open(filename, 'w') as wav_file:
        wav_file.setnchannels(1)        # 单声道
        wav_file.setsampwidth(2)        # 16-bit
        wav_file.setframerate(samplerate)

        for i in range(nframes):
            # 计算当前时刻的采样值，范围在 [-1.0, 1.0]
            value = math.sin(2 * math.pi * freq * (i / samplerate))
            # 将浮点数映射到 16-bit PCM 的范围 [-32768, 32767]
            pcm_value = int(value * 32767)
            # 将整数值打包为二进制格式并写入文件
            wav_file.writeframes(struct.pack('<h', pcm_value))

# 生成一个持续2秒的440Hz正弦波
create_sine_wave(freq=440, duration=2, filename="sine.wav")