## 🎵 TXAC Codec

Lossy audio compression codec with AVX optimization.

-----

## 📦 Programs

### 1\. **txac\_input.c** - Encoder (txacinput.exe)

Converts any audio format to `.txac`.

**Features:**

  * ✅ Accepts any file name via arguments.
  * ✅ Supports 16-bit WAV (automatically converts to 32-bit).
  * ✅ Supports native 32-bit WAV.
  * ✅ **Supports ANY format** (FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, etc.).
  * ✅ Automatic conversion via FFmpeg for non-WAV formats.
  * ✅ All processing done in RAM (no intermediate files).
  * ✅ AVX optimization for fast processing.

**Compile:**

```bash
# Windows with Zig
zig cc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txacinput.exe

# Linux
gcc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txacinput
```

**Usage:**

```bash
# WAV (processed directly, no conversion)
txacinput input.wav output.txac

# FLAC (converted via ffmpeg)
txacinput input.flac output.txac

# MP3 (converted via ffmpeg)
txacinput input.mp3 output.txac

# M4A/AAC (converted via ffmpeg)
txacinput input.m4a output.txac

# OGG Vorbis (converted via ffmpeg)
txacinput input.ogg output.txac

# OPUS (converted via ffmpeg)
txacinput input.opus output.txac

# Any other format supported by ffmpeg
txacinput input.wma output.txac
```

-----

### 2\. **txac\_output.c** - Decoder (txacoutput.exe)

Converts `.txac` to WAV.

**Features:**

  * ✅ Accepts any file name via arguments.
  * ✅ Specifies sample rate and channels via command line.
  * ✅ All processing done in RAM.
  * ✅ AVX optimization for fast decompression.

**Compile:**

```bash
# Windows with Zig
zig cc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txacoutput.exe

# Linux
gcc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txacoutput
```

**Usage:**

```bash
# Stereo 44100 Hz
txacoutput audio.txac output.wav 44100 2

# Mono 48000 Hz
txacoutput audio.txac output.wav 48000 1
```

-----

### 3\. **txacplay.c** - Player (txacplay.exe)

Plays `.txac` directly.

**Features:**

  * ✅ Real-time playback.
  * ✅ AVX optimization for fast decoding.
  * ✅ Interactive controls (pause, seek).
  * ✅ Automatic loop.

**Compile:**

```bash
# Windows
zig cc txacplay.c -std=gnu99 -pthread -O3 -mavx2 -lm -lole32 -o txacplay.exe

# Linux
gcc txacplay.c -std=gnu99 -lasound -pthread -O3 -mavx2 -lm -o txacplay
```

**Usage:**

```bash
# Stereo 44100 Hz
txacplay audio.txac 44100 2

# Mono 48000 Hz
txacplay audio.txac 48000 1
```

**Controls:**

  * **SPACE** - Pause/Resume.
  * **X** - Rewind 5 seconds.
  * **C** - Forward 5 seconds.
  * **Q** - Quit.

-----

## 🔧 FLAC Support

To convert FLAC, you need to have **FFmpeg** installed:

### Windows:

1.  Download from: [https://ffmpeg.org/download.html](https://ffmpeg.org/download.html).
2.  Extract and add to PATH.
3.  Test: `ffmpeg -version`.

### Linux:

```bash
# Ubuntu/Debian
sudo apt install ffmpeg

# Arch
sudo pacman -S ffmpeg
```

**How it works:**

  * The encoder automatically detects `.flac` files.
  * Converts to temporary WAV using FFmpeg.
  * Processes normally.
  * Removes the temporary file.

-----

## 📊 Processing Pipeline

### Encoder:

```
FLAC/WAV → [16→32 bit Conversion] → [dB Reduction with AVX] → 
[Compression ^~] → [4-bit encoding] → .txac
```

### Decoder:

```
.txac → [4-bit decoding] → [Decompression with AVX] → 
[Gain Application with AVX] → WAV 32-bit
```

### Player:

```
.txac → [4-bit decoding] → [Decompression] → 
[Normalization with AVX] → Sokol Audio
```

-----

## ⚡ AVX Optimizations

All critical operations use AVX (processes 8 samples at a time):

1.  **Encoder:**
      * dB Reduction (int32 → float → multiply → int32).
      * 16-bit → 32-bit Conversion.
2.  **Decoder:**
      * Gain application during decompression.
      * Vectorized clipping.
3.  **Player:**
      * Real-time int32 → float32 normalization.

-----

## 📝 .txac Format

  * **Extension:** `.txac` (Text Audio Codec).
  * **Structure:** 4 bits per symbol (16 symbols: `0-9`, `^`, `~`, `(`, `)`, `-`, `,`).
  * **Compression:**
      * `^` = consecutive repetition (e.g., `100^50` = 50x the value 100).
      * `~` = programmed sniper (e.g., `100~5,x,x,x,x,x` = value 100 repeats after 5 values).
  * **Reduction:** 110 dB (default) applied on input.
  * **Gain:** 110 dB applied on output.

-----

## 💡 Complete Examples

### Complete Workflow:

```bash
# 1. Encode (WAV → TXAC)
txacinput music.wav music.txac 110

# 2. Play (TXAC direct)
txacplay music.txac 44100 2

# 3. Decode (TXAC → WAV)
txacoutput music.txac restored.wav 44100 2
```

### With FLAC:

```bash
# Encode FLAC directly
txacinput album.flac album.txac 110

# Play
txacplay album.txac 44100 2
```

-----

## 🎯 Advantages

  * ✅ **No intermediate files** - all in RAM.
  * ✅ **Automatic 16/32 bit support** - converts transparently.
  * ✅ **FLAC support** - via FFmpeg.
  * ✅ **AVX Optimization** - 8x faster processing.
  * ✅ **Integrated Player** - plays directly.
  * ✅ **Flexible command line** - any file, any configuration.
  * ✅ **Very versatile for compression** - compression methods like zip will actually make txac smaller (but won't run).

-----

## ⚙️ Requirements

  * **CPU:** AVX2 Support (Intel Sandy Bridge+, AMD Bulldozer+).
  * **Compiler:** GCC or Zig.
  * **Optional:** FFmpeg (for FLAC).
  * **Player:** sokol\_audio.h (included in txacplay.c).

-----

## 🐛 Troubleshooting

**"Erro: ffmpeg não encontrado"**

  * Install FFmpeg and add it to PATH.

**"Illegal instruction"**

  * Your CPU does not support AVX2, compile without `-mavx2`.

**"Audio cb channels não igual"**

  * Check if you are passing the correct number of channels.

**Flickering in the player**


  * Increase the buffer: modify `.buffer_frames` in the code.
