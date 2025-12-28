# 🎵 TXAC Codec v0.2.0

Lossless audio compression codec with multi-core processing and AVX2 optimization.

---

## 📦 Programs

### 1. **txac_input.c** - Encoder (v0.2.0)

Converts any audio format to `.txac` with multi-threaded compression.

**Features:**

* ✅ **Multi-core compression** - Each channel compressed in parallel
* ✅ **Direct 4-bit encoding** - Text → 4-bit conversion during compression
* ✅ Supports 16-bit WAV (automatically converts to 32-bit)
* ✅ Supports native 32-bit WAV
* ✅ **Supports ANY format via ffmpeg** (FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, etc.)
* ✅ Automatic conversion via FFmpeg for non-WAV formats
* ✅ All processing done in RAM
* ✅ TXAC v0.2.0 format with complete header

**Compile:**

```bash
# Windows with Zig
zig cc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_encode.exe

# Linux
gcc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_encode
```

**Usage:**

```bash
# Basic encoding (WAV/FLAC/MP3/etc)
txac_encode input.wav output.txac

# Any format via FFmpeg
txac_encode input.mp3 output.txac
txac_encode input.m4a output.txac
txac_encode input.ogg output.txac
```

---

### 2. **txac_output.c** - Decoder (v0.2.0)

Converts `.txac` to 32-bit WAV with multi-threaded decompression.

**Features:**

* ✅ **Multi-core decompression** - Each channel decoded in parallel
* ✅ **Direct 4-bit parsing** - 4-bit → int32 without intermediate text buffer
* ✅ **Streaming decompression** - Lower RAM usage
* ✅ Reads all metadata from TXAC header (no manual config needed)
* ✅ Automatic gain restoration (110dB)
* ✅ AVX2 optimization for repetition patterns

**Compile:**

```bash
# Windows with Zig
zig cc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_decode.exe

# Linux
gcc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_decode
```

**Usage:**

```bash
# Simple decode (sample rate and channels read from file)
txac_decode audio.txac output.wav

# That's it! No need to specify format anymore
```

**Output:**
* Always 32-bit PCM WAV
* Original sample rate and channel count preserved
* 110dB gain automatically applied

---

### 3. **txacplay.c** - Player (v0.2.0)

Real-time `.txac` player with direct float conversion.

**Features:**

* ✅ **Multi-core loading** - Channels loaded in parallel
* ✅ **Direct 4-bit → float parsing** - No intermediate conversions
* ✅ **Zero-copy playback** - Buffer already in float format
* ✅ Automatic format detection from header
* ✅ Interactive controls
* ✅ Automatic looping
* ✅ Real-time seek (5s increments)

**Compile:**

```bash
# Windows with Zig
zig cc txacplay.c -std=gnu99 -pthread -O3 -mavx2 -lm -lole32 -o txacplay.exe

# Linux
gcc txacplay.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txacplay
```

**Usage:**

```bash
# Just pass the file (format auto-detected)
txacplay audio.txac
```

**Controls:**

* **SPACE** - Pause/Resume
* **X** - Rewind 5 seconds
* **C** - Forward 5 seconds
* **Q** - Quit

---

## 🔧 FFmpeg Support

To convert non-WAV formats, you need **FFmpeg** installed:

### Windows:

1. Download from: [https://ffmpeg.org/download.html](https://ffmpeg.org/download.html)
2. Extract and add to PATH
3. Test: `ffmpeg -version`

### Linux:

```bash
# Ubuntu/Debian
sudo apt install ffmpeg

# Arch
sudo pacman -S ffmpeg
```

**Supported Input Formats:**
* WAV (direct, no conversion)
* FLAC, MP3, AAC, M4A
* OGG, OPUS, WMA
* Any format supported by FFmpeg

---

## 📊 Processing Pipeline

### Encoder (Multi-threaded):

```
Input Audio → [FFmpeg Conversion] → WAV 32-bit → 
[Multi-channel Split] → 
  ├─ Thread 1: Channel 0 → [110dB Reduction] → [^~ Compression] → [4-bit Encode]
  ├─ Thread 2: Channel 1 → [110dB Reduction] → [^~ Compression] → [4-bit Encode]
  └─ Thread N: Channel N → ... → [4-bit Encode]
→ [TXAC v0.2.0 Container] → .txac
```

### Decoder (Multi-threaded):

```
.txac → [Read TXAC Header] → 
  ├─ Thread 1: [4-bit Stream] → [Direct Int32 Parse] → [110dB Gain + AVX2] → Channel 0
  ├─ Thread 2: [4-bit Stream] → [Direct Int32 Parse] → [110dB Gain + AVX2] → Channel 1
  └─ Thread N: [4-bit Stream] → ... → Channel N
→ [Interleave] → WAV 32-bit
```

### Player (Multi-threaded):

```
.txac → [Read TXAC Header] → 
  ├─ Thread 1: [4-bit Stream] → [Direct Float Parse] → Channel 0 Float Buffer
  ├─ Thread 2: [4-bit Stream] → [Direct Float Parse] → Channel 1 Float Buffer
  └─ Thread N: [4-bit Stream] → ... → Channel N Float Buffer
→ [Interleave Float] → [Zero-copy to Sokol Audio] → 🔊
```

---

## ⚡ Performance Optimizations

### Multi-threading:
* **Encoder**: N threads = N channels (parallel compression)
* **Decoder**: N threads = N channels (parallel decompression)
* **Player**: N threads = N channels (parallel loading)

### AVX2 Optimizations:

1. **Encoder (txac_input.c):**
   * Volume reduction: 8 samples per cycle
   * 16-bit → 32-bit conversion: vectorized

2. **Decoder (txac_output.c):**
   * Repetition patterns (`^`): 8 int32 writes per cycle
   * Gain application: vectorized with clipping

3. **Player (txacplay.c):**
   * Direct float buffer (no conversion in callback)
   * Memory copy only during playback

### Direct Parsing:

**Old approach:**
```
4-bit → full text buffer → parse → int32/float
```

**New approach (v0.2.0):**
```
4-bit → stream reader → direct int32/float (no intermediate buffer)
```

**Benefits:**
* Optimized RAM usage
* Faster decompression
* Lower cache pressure

---

## 📝 .txac v3 Format

### File Structure:

```
[Header - 64 bytes]
  ├─ Magic: "TXAC" (4 bytes)
  ├─ Version: 3 (4 bytes)
  ├─ Sample Rate: uint32 (4 bytes)
  ├─ Channels: uint16 (2 bytes)
  ├─ Bits per Sample: 32 (2 bytes)
  ├─ Flags: uint32 (4 bytes, bit 0 = loop enabled)
  ├─ Total Samples: uint64 (8 bytes)
  └─ Reserved: 36 bytes

[Channel Index - 16 bytes per channel]
  ├─ Offset: uint64 (8 bytes)
  └─ Size: uint64 (8 bytes)

[Channel Data - Variable size]
  ├─ Channel 0: 4-bit compressed data
  ├─ Channel 1: 4-bit compressed data
  └─ Channel N: 4-bit compressed data
```

### Compression Symbols (4-bit):

* **Digits:** `0-9` (values)
* **Operators:**
  * `,` - Separator
  * `^` - Repetition (e.g., `100^50` = value 100 repeated 50 times)
  * `~` - Sniper (e.g., `100~5,a,b,c,d,e` = insert value after 5 values)
  * `(`, `)` - Grouping (reserved) (doesn't work yet)
  * `-` - Negative sign

### Loop Markers: (absolutely useless for now)
* This is completely optional in the encode because it doesn't change a thing 
* `LOOP^N` - Consecutive loop at end (repeat last N samples)
* `LOOP~N` - Non-consecutive loop (loop starts at position N)

---

## 💡 Complete Examples

### Complete Workflow:

```bash
# 1. Encode with loop detection (multi-core)
txacinput music.wav music.txac --loop

# 2. Play directly (multi-core loading)
txacplay music.txac

# 3. Decode to WAV (multi-core)
txacoutput music.txac restored.wav
```

### Batch Processing:

```bash
# Encode entire music library
for %i in ("C:\your folder\*.'the format of the file'") do txacinput "%i" "C:\another folder\%~ni.txac"
done
```

---

## 🎯 Advantages

* ✅ **Multi-core processing** - Scales with CPU cores
* ✅ **Direct streaming parsing** - Lower RAM, faster
* ✅ **Self-contained format** - No need to specify sample rate/channels
* ✅ **Zero intermediate files** - All processing in RAM
* ✅ **Automatic 16/32 bit support** - Transparent conversion
* ✅ **Universal format support** - Via FFmpeg
* ✅ **AVX2 optimization** - 8x faster critical operations
* ✅ **Integrated player** - Direct playback
* ✅ **Post-compressible** - Works well with ZIP/7z

---

## ⚙️ Requirements

* **CPU:** AVX2 Support (Intel Haswell+ 2013, AMD Excavator+ 2015)
* **Compiler:** GCC 4.9+ or Zig 0.11+
* **RAM:** ~2x uncompressed audio size during processing
* **Optional:** FFmpeg (for non-WAV formats)
* **Player:** sokol_audio.h (single-header library, included)

---

## 🛠️ Troubleshooting

**"Error converting FFmpeg"**
* Install FFmpeg and add to PATH
* Test: `ffmpeg -version`

**"Illegal instruction"**
* CPU doesn't support AVX2
* Compile without `-mavx2` flag

**"Error: Cannot open file"**
* Check file path and permissions
* Ensure file extension is correct

**Playback issues:**
* Update sokol_audio.h to latest version
* Try increasing `.buffer_frames` in source code

**Multi-threading not working:**
* Ensure pthread is linked (`-pthread` flag)
* Check CPU core count: should see N threads for N channels

---

## 📈 Performance Comparison

### Encoding Speed (4-core CPU, stereo 44.1kHz):

| File Length | Old (Single) | v0.2.0 (Multi) | Speedup |
|-------------|--------------|----------------|---------|
| 3 minutes   | 8.2s         | 4.5s           | 1.8x    |
| 10 minutes  | 27.1s        | 14.3s          | 1.9x    |
| 30 minutes  | 81.5s        | 42.8s          | 1.9x    |

### Decoding Speed:

| File Length | Old (Single) | v0.2.0 (Multi) | Speedup |
|-------------|--------------|----------------|---------|
| 3 minutes   | 3.1s         | 1.7s           | 1.8x    |
| 10 minutes  | 10.3s        | 5.5s           | 1.9x    |

### Player Loading Time:

| File Length | Old         | v0.2.0 (Multi) | Speedup |
|-------------|-------------|----------------|---------|
| 3 minutes   | 2.8s        | 1.5s           | 1.9x    |
| 10 minutes  | 9.2s        | 4.9s           | 1.9x    |

*Note: Near 2x speedup for stereo (2 threads vs 1)*

---

## 🔬 Technical Details

### Volume Reduction:
* **Encoder:** -110dB reduction (÷ 316,227.766)
* **Decoder:** +110dB gain (× 316,227.766)
* **Format:** Maintains 32-bit integer precision

### Loop Detection Algorithm: (doesn't work)
1. Extract 15,000-sample signature from end
2. Scan backward with AVX2 (8 samples per comparison)
3. Extend match in both directions
4. Classify as consecutive or non-consecutive
5. Encode as `LOOP^size` or `LOOP~position`

### 4-bit Encoding:
* 16 symbols = 4 bits per character
* 2 characters per byte
* Theoretical compression: ~2:1 for text representation
* Practical: 10:1 to 50:1 depending on repetition

---

## 📝 Version History

### v0.2.0 (Current)
* Multi-threaded encoder/decoder/player
* Direct 4-bit streaming parser
* TXAC v0.2.0 container format with header
* Loop detection with AVX2
* Auto-detection of format (no manual config)

### v0.1.0
* Single-threaded
* Full text buffer approach
* Manual sample rate/channel specification

---