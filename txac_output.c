/*
TXAC Decoder v3.0 - Multi-core Direct 4bit→Int32
- Lê formato TXAC v3 (multicanal com header completo)
- Parsing direto: 4-bit → int32 (sem etapa intermediária de texto)
- Multi-threading para descompressão paralela
- Aplica ganho de 110dB durante parsing
- Suporta detecção de loops

Compilar:
    zig cc txac_output.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_decode.exe
    
Uso:
    txac_decode input.txac output.wav
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <immintrin.h>

#define TXAC_MAGIC "TXAC"
#define MAX_CHANNELS 8
#define GAIN_DB 110.0
#define AMPLITUDE_FACTOR 316227.76601683795  // pow(10.0, 110.0 / 20.0)
#define INT32_MAX_VAL 2147483647
#define INT32_MIN_VAL -2147483648

const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t flags;
    uint64_t total_samples;
} TXACHeader;

// ============================================================================
// BUFFER INT32 COM GANHO APLICADO
// ============================================================================
typedef struct {
    int32_t *data;        
    uint64_t capacity;
    uint64_t count;
} BufferInt32;

void init_buffer_int32(BufferInt32 *buf, uint64_t initial_capacity) {
    buf->capacity = initial_capacity;
    buf->count = 0;
    buf->data = (int32_t*)malloc(buf->capacity * sizeof(int32_t));
    if (!buf->data) {
        fprintf(stderr, "Error: Failed to allocate RAM for buffer\n");
        exit(1);
    }
}

void ensure_buffer_capacity(BufferInt32 *buf, uint64_t required) {
    if (buf->count + required >= buf->capacity) {
        uint64_t new_cap = buf->capacity * 2;
        while (buf->count + required >= new_cap) new_cap *= 2;
        
        int32_t *new_ptr = (int32_t*)realloc(buf->data, new_cap * sizeof(int32_t));
        if (!new_ptr) {
            fprintf(stderr, "Error: Insufficient memory\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

void push_value_int32(BufferInt32 *buf, int32_t value) {
    ensure_buffer_capacity(buf, 1);
    buf->data[buf->count++] = value;
}

// ============================================================================
// STREAM 4-BIT READER
// ============================================================================
typedef struct {
    uint8_t *raw_data;
    size_t byte_count;
    size_t byte_pos;
    int nibble_pos;  // 0 = high nibble, 1 = low nibble
} Stream4Bit;

void init_stream(Stream4Bit *s, uint8_t *data, size_t size) {
    s->raw_data = data;
    s->byte_count = size;
    s->byte_pos = 0;
    s->nibble_pos = 0;
}

char read_next_char(Stream4Bit *s) {
    if (s->byte_pos >= s->byte_count) return '\0';
    
    uint8_t byte = s->raw_data[s->byte_pos];
    char c;
    
    if (s->nibble_pos == 0) {
        c = simbolos[(byte >> 4) & 0x0F];
        s->nibble_pos = 1;
    } else {
        c = simbolos[byte & 0x0F];
        s->nibble_pos = 0;
        s->byte_pos++;
    }
    
    return c;
}

void read_token_stream(Stream4Bit *s, char *buffer, size_t max_len) {
    size_t idx = 0;
    char c;
    
    while ((c = read_next_char(s)) != '\0' && c != ',') {
        if (idx < max_len - 1) {
            buffer[idx++] = c;
        }
    }
    buffer[idx] = '\0';
}

// ============================================================================
// APLICAÇÃO DE GANHO COM CLIPPING
// ============================================================================
int32_t apply_gain_and_clip(double value) {
    double boosted = value * AMPLITUDE_FACTOR;
    
    if (boosted > INT32_MAX_VAL) return INT32_MAX_VAL;
    if (boosted < INT32_MIN_VAL) return INT32_MIN_VAL;
    return (int32_t)boosted;
}

// ============================================================================
// DESCOMPRESSÃO DIRETA PARA INT32 (COM GANHO)
// ============================================================================
typedef struct {
    int channel_id;
    uint8_t *compressed_4bit;
    size_t compressed_size;
    BufferInt32 *output_buffer;
    pthread_t thread;
    volatile int finished;
} ChannelDecoder;

void process_stream_to_int32(ChannelDecoder *dec, Stream4Bit *stream, uint64_t *sample_idx, int recursion_depth) {
    if (recursion_depth > 100) return;
    
    char token[128];
    
    while (stream->byte_pos < stream->byte_count || stream->nibble_pos > 0) {
        read_token_stream(stream, token, 128);
        
        if (token[0] == '\0') continue;
        if (token[0] == '(' || token[0] == ')') continue;
        
        // Detecta LOOP
        if (strncmp(token, "LOOP", 4) == 0) {
            printf("  [Channel %d] Loop detected, ending stream\n", dec->channel_id);
            break;
        }
        
        double num_d;
        int rep;
        int32_t val_int32;
        
        // Caso 1: Repetição (valor^repeticoes)
        if (sscanf(token, "%lf^%d", &num_d, &rep) == 2) {
            val_int32 = apply_gain_and_clip(num_d);
            
            ensure_buffer_capacity(dec->output_buffer, rep);
            
            // Otimização AVX2 para repetições
            if (rep >= 8) {
                __m256i v_val = _mm256_set1_epi32(val_int32);
                int avx_blocks = rep / 8;
                
                for (int i = 0; i < avx_blocks; i++) {
                    _mm256_storeu_si256((__m256i*)&dec->output_buffer->data[dec->output_buffer->count], v_val);
                    dec->output_buffer->count += 8;
                    (*sample_idx) += 8;
                }
                
                // Restante
                int remaining = rep % 8;
                for (int i = 0; i < remaining; i++) {
                    dec->output_buffer->data[dec->output_buffer->count++] = val_int32;
                    (*sample_idx)++;
                }
            } else {
                for (int i = 0; i < rep; i++) {
                    dec->output_buffer->data[dec->output_buffer->count++] = val_int32;
                    (*sample_idx)++;
                }
            }
            
            if (recursion_depth > 0) return;
        }
        // Caso 2: Sniper (valor~distancia)
        else if (sscanf(token, "%lf~%d", &num_d, &rep) == 2) {
            val_int32 = apply_gain_and_clip(num_d);
            push_value_int32(dec->output_buffer, val_int32);
            (*sample_idx)++;
            
            for (int i = 0; i < rep; i++) {
                process_stream_to_int32(dec, stream, sample_idx, recursion_depth + 1);
            }
            
            push_value_int32(dec->output_buffer, val_int32);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
        // Caso 3: Valor simples
        else if (sscanf(token, "%lf", &num_d) == 1) {
            val_int32 = apply_gain_and_clip(num_d);
            push_value_int32(dec->output_buffer, val_int32);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
    }
}

void *decoder_thread_func(void *arg) {
    ChannelDecoder *dec = (ChannelDecoder*)arg;
    uint64_t sample_idx = 0;
    
    printf("  [Channel %d] Decompressing 4bit to int32 directly...\n", dec->channel_id);
    
    Stream4Bit stream;
    init_stream(&stream, dec->compressed_4bit, dec->compressed_size);
    process_stream_to_int32(dec, &stream, &sample_idx, 0);
    
    printf("  [Channel %d] %llu samples decoded\n", dec->channel_id, (unsigned long long)sample_idx);
    dec->finished = 1;
    return NULL;
}

// ============================================================================
// INTERCALAÇÃO DE CANAIS
// ============================================================================
int32_t *intercalar_canais(BufferInt32 *channels, int num_channels, uint64_t *total_samples_out) {
    printf("\nInterleaving channels...\n");
    
    uint64_t frames = channels[0].count;
    for (int i = 1; i < num_channels; i++) {
        if (channels[i].count < frames) {
            frames = channels[i].count;
        }
    }
    
    uint64_t total_samples = frames * num_channels;
    int32_t *interleaved = (int32_t*)malloc(total_samples * sizeof(int32_t));
    
    if (!interleaved) {
        fprintf(stderr, "Fatal: No RAM for interleaved buffer\n");
        exit(1);
    }
    
    for (uint64_t f = 0; f < frames; f++) {
        for (int c = 0; c < num_channels; c++) {
            interleaved[f * num_channels + c] = channels[c].data[f];
        }
    }
    
    *total_samples_out = total_samples;
    double size_mb = (double)(total_samples * sizeof(int32_t)) / (1024.0 * 1024.0);
    printf("Interleaved: %.2f MB (%llu samples)\n", size_mb, total_samples);
    
    return interleaved;
}

// ============================================================================
// SALVAR WAV 32-BIT
// ============================================================================
void salvar_wav_32bit(const char *filename, int32_t *samples, uint64_t sample_count, 
                      uint32_t sample_rate, uint16_t channels) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("Error creating WAV");
        return;
    }
    
    printf("\nSaving WAV: %d Hz, %d channels, 32-bit...\n", sample_rate, channels);
    
    uint16_t bits_per_sample = 32;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);
    uint32_t data_size = sample_count * sizeof(int32_t);
    uint32_t file_size = 36 + data_size;
    
    // Header WAV
    uint8_t header[44];
    memset(header, 0, 44);
    
    memcpy(header, "RIFF", 4);
    memcpy(header + 4, &file_size, 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    
    uint32_t subchunk1_size = 16;
    uint16_t audio_format = 1;
    
    memcpy(header + 16, &subchunk1_size, 4);
    memcpy(header + 20, &audio_format, 2);
    memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &sample_rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits_per_sample, 2);
    memcpy(header + 36, "data", 4);
    memcpy(header + 40, &data_size, 4);
    
    fwrite(header, 1, 44, f);
    fwrite(samples, sizeof(int32_t), sample_count, f);
    
    fclose(f);
    
    double size_mb = (double)(data_size) / (1024.0 * 1024.0);
    printf("WAV saved: %s (%.2f MB)\n", filename, size_mb);
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input.txac> <output.wav>\n", argv[0]);
        printf("Example: %s audio.txac audio.wav\n", argv[0]);
        return 1;
    }
    
    const char *input = argv[1];
    const char *output = argv[2];
    
    printf("TXAC output v0.2.0\n");
    printf("Input:  %s\n", input);
    printf("Output: %s\n\n", output);
    
    // Abre arquivo TXAC
    FILE *f = fopen(input, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s\n", input);
        return 1;
    }
    
    // Lê e valida header
    char magic[4];
    fread(magic, 1, 4, f);
    
    if (memcmp(magic, TXAC_MAGIC, 4) != 0) {
        fprintf(stderr, "Error: Invalid TXAC file (bad magic)\n");
        fclose(f);
        return 1;
    }
    
    TXACHeader header;
    uint32_t version;
    
    fread(&version, 4, 1, f);
    fread(&header.sample_rate, 4, 1, f);
    fread(&header.channels, 2, 1, f);
    fread(&header.bits_per_sample, 2, 1, f);
    fread(&header.flags, 4, 1, f);
    fread(&header.total_samples, 8, 1, f);
    
    printf(" TXAC Info:\n");
    printf(" Version: %d\n", version);
    printf(" Sample rate: %d Hz\n", header.sample_rate);
    printf(" Channels: %d\n", header.channels);
    printf(" Bits per sample: %d\n", header.bits_per_sample);
    printf(" Total samples: %llu\n", (unsigned long long)header.total_samples);
    printf(" Loop enabled: %s\n\n", (header.flags & 1) ? "Yes" : "No");
    
    if (header.channels > MAX_CHANNELS) {
        fprintf(stderr, "Error: Too many channels (%d > %d)\n", header.channels, MAX_CHANNELS);
        fclose(f);
        return 1;
    }
    
    // Pula reserved bytes
    fseek(f, 36, SEEK_CUR);
    
    // Lê índice de canais
    uint64_t offsets[MAX_CHANNELS];
    uint64_t sizes[MAX_CHANNELS];
    
    for (int i = 0; i < header.channels; i++) {
        fread(&offsets[i], 8, 1, f);
        fread(&sizes[i], 8, 1, f);
    }
    
    // Prepara decodificadores multi-thread
    ChannelDecoder decoders[MAX_CHANNELS];
    BufferInt32 channel_buffers[MAX_CHANNELS];
    
    printf("Starting multi-threaded decompression...\n");
    
    for (int i = 0; i < header.channels; i++) {
        init_buffer_int32(&channel_buffers[i], header.total_samples / header.channels);
        
        decoders[i].channel_id = i;
        decoders[i].output_buffer = &channel_buffers[i];
        decoders[i].compressed_size = sizes[i];
        decoders[i].finished = 0;
        
        // Lê dados 4-bit brutos
        decoders[i].compressed_4bit = malloc(sizes[i]);
        if (!decoders[i].compressed_4bit) {
            fprintf(stderr, "Error: Cannot allocate memory for channel %d\n", i);
            fclose(f);
            return 1;
        }
        
        fseek(f, offsets[i], SEEK_SET);
        fread(decoders[i].compressed_4bit, 1, sizes[i], f);
        
        pthread_create(&decoders[i].thread, NULL, decoder_thread_func, &decoders[i]);
    }
    
    // Aguarda todas as threads
    for (int i = 0; i < header.channels; i++) {
        pthread_join(decoders[i].thread, NULL);
        free(decoders[i].compressed_4bit);
    }
    
    fclose(f);
    
    // Intercala canais
    uint64_t total_samples;
    int32_t *interleaved = intercalar_canais(channel_buffers, header.channels, &total_samples);
    
    // Salva WAV
    salvar_wav_32bit(output, interleaved, total_samples, header.sample_rate, header.channels);
    
    // Cleanup
    for (int i = 0; i < header.channels; i++) {
        free(channel_buffers[i].data);
    }
    free(interleaved);
    
    printf("\nDecoding completed successfully!\n");
    printf("Audio duration: %.2f seconds\n", 
           (double)total_samples / (header.sample_rate * header.channels));
    
    return 0;
}