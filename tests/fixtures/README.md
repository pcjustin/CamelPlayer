# Test fixtures

Encoded audio the tests read, so that neither the build nor the tests need FFmpeg. Every file was written by FFmpeg 9.0.2 through `generator/generate.cpp`. "The test signal" is `makeSignal` in `src/Tools/testaudio/TestAudio.cpp`, a deterministic sine and noise that reaches every bit.

| File | What it carries | Bytes | SHA-256 |
|---|---|---|---|
| `aac.m4a` | AAC-LC, 256 kbit/s, M4A, 3 s of two modulated tones, 44.1 kHz stereo | 87,196 | `a9566026a163661c800ba9a33028c65512039dc3ed2648eca221f9cdf69225f4` |
| `aac.aac` | The same as ADTS | 86,783 | `cc6c1ad549766e8d1faea009250fc4a8a267af4e39188f1ed24ae180d5d57c76` |
| `aac-loud.m4a` | The same M4A at 1.6 times the level, clipped to 24 bits before encoding | 90,065 | `495e023c41aa8c874afc1a7ac2bb8c17ac8568f8d8cd9cbe968ceef2843116a1` |
| `aac-short.m4a` | AAC-LC, 256 kbit/s, M4A, 0.1 s of the test signal, 44.1 kHz stereo | 3,961 | `ba0ae0933f95b37e80e9f2e9857603eba273a4df25c5664980daf25a83ed6305` |
| `aac-short.aac` | The same as ADTS | 3,173 | `b9ebcc13ef02061695c6078ecd4772cdbbef8f9dee84d78379eb06ea0a3afc48` |
| `alac-16-44.1k.m4a` | ALAC, 0.5 s of the test signal, 16-bit, 44.1 kHz stereo | 83,850 | `1e7cbcf6f39ec05cbc7df46225a35d37b4aaacddf614be3aaaf38cba3b633345` |
| `alac-24-96k.m4a` | ALAC, 0.25 s of the test signal, 24-bit, 96 kHz stereo | 138,393 | `c9451b62358cd4c7eec83d64b9d481cc9dd06d8feecfae0b240419121649389e` |
| `alac-24-192k.m4a` | ALAC, 0.25 s of the test signal, 24-bit, 192 kHz stereo | 275,472 | `bd733b6f2171045acbfa4dc9b38eb83fd2132bb18dfa9ec1e33dbb8e9440db8a` |
| `alac-short.m4a` | ALAC, 0.1 s of the test signal, 16-bit, 44.1 kHz stereo | 17,417 | `768ddfe046842bfb758e9b01a0e996db68277ca0522b2585c4d9c2a0f219b9c5` |
| `dts-cd.wav` | DTS-CD: 0.5 s of six tones, 5.1 at 1234.8 kbit/s, 14-bit words in 44.1 kHz 16-bit stereo WAVE | 88,108 | `301cbd7493ddee9a28adb312e322fad9b8e419b156c6957d80171abd513f491f` |
| `dts-cd.f32` | FFmpeg's decode of that stream: 32-bit little-endian float, interleaved L R C LFE Ls Rs | 528,384 | `df8dbcc0303b4a55ee8309cf1b0957b53ea136fdb3d1b48dd71867b8cc32e0c9` |

`dts-cd.f32` is FFmpeg's decode as the DTS tests stored it, compared with `dts_core` at -100 dBFS. The tests' ADPCM files are written by libsndfile while they run, and their FLAC, WAV, AIFF and MP3 files by libFLAC, CamelPlayer's own writers and LAME.

To write them again, install the generator's own dependencies and run its build from a Developer Command Prompt, in `generator`:

```bash
vcpkg install --triplet x64-windows-static-md
```

```bash
build.cmd
```

AAC and ALAC files depend on the encoder's version, so new ones can differ from the hashes above. Update this list with them.
