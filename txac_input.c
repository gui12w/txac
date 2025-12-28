/*
TXAC Encoder v3.1 - 32-bit Support
- FFmpeg agora converte para 32-bit (pcm_s32le)
- Suporte nativo para leitura de WAV 32-bit int
- Mantém otimizações AVX2

Compilar:
    zig cc txac_input.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_encode.exe
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <immintrin.h>

#define TXAC_MAGIC "TXAC"
#define TXAC_VERSION 3
#define DB_REDUCTION 110.0
#define MAX_CHANNELS 8
#define GROWTH_FACTOR 2

// Símbolos 4-bit
const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

typedef struct {
    int32_t *samples;
    size_t count;
    size_t capacity;
} Channel;

typedef struct {
    uint8_t *data;  // Dados 4-bit compactados
    size_t byte_count;
    size_t capacity;
    int high_nibble;  // -1 ou valor do nibble alto
} Binary4BitBuffer;

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t flags;
    uint64_t total_samples;
} TXACHeader;

typedef struct {
    Channel *channel;
    Binary4BitBuffer *output;
    int channel_id;
    int enable_loop_compression;
} ThreadData;

// ============================================================================
// BUFFER 4-BIT DIRETO
// ============================================================================

void init_4bit_buffer(Binary4BitBuffer *buf) {
    buf->capacity = 1024 * 1024;
    buf->byte_count = 0;
    buf->data = (uint8_t*)malloc(buf->capacity);
    buf->high_nibble = -1;
    if (!buf->data) {
        fprintf(stderr, "Error allocating 4-bit buffer.\n");
        exit(1);
    }
}

void ensure_4bit_capacity(Binary4BitBuffer *buf, size_t required_bytes) {
    if (buf->byte_count + required_bytes >= buf->capacity) {
        size_t new_cap = buf->capacity * GROWTH_FACTOR;
        while (buf->byte_count + required_bytes >= new_cap) {
            new_cap *= GROWTH_FACTOR;
        }
        uint8_t *new_ptr = (uint8_t*)realloc(buf->data, new_cap);
        if (!new_ptr) {
            fprintf(stderr, "Error reallocating 4-bit buffer.\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

int char_para_4bit(char c) {
    for (int i = 0; i < 16; i++) {
        if (simbolos[i] == c) return i;
    }
    return -1;
}

void write_char_4bit(Binary4BitBuffer *buf, char c) {
    int nibble = char_para_4bit(c);
    if (nibble == -1) return;
    
    if (buf->high_nibble == -1) {
        buf->high_nibble = nibble;
    } else {
        ensure_4bit_capacity(buf, 1);
        uint8_t byte = (buf->high_nibble << 4) | nibble;
        buf->data[buf->byte_count++] = byte;
        buf->high_nibble = -1;
    }
}

void write_string_4bit(Binary4BitBuffer *buf, const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        write_char_4bit(buf, str[i]);
    }
}

void finalize_4bit_buffer(Binary4BitBuffer *buf) {
    if (buf->high_nibble != -1) {
        ensure_4bit_capacity(buf, 1);
        uint8_t byte = (buf->high_nibble << 4);
        buf->data[buf->byte_count++] = byte;
        buf->high_nibble = -1;
    }
}

// ============================================================================
// CANAL
// ============================================================================

void init_channel(Channel *ch, size_t initial_capacity) {
    ch->capacity = initial_capacity;
    ch->count = 0;
    ch->samples = (int32_t*)malloc(ch->capacity * sizeof(int32_t));
    if (!ch->samples) {
        fprintf(stderr, "Error allocating channel\n");
        exit(1);
    }
}

void ensure_channel_capacity(Channel *ch, size_t required) {
    if (ch->count + required >= ch->capacity) {
        size_t new_cap = ch->capacity * GROWTH_FACTOR;
        while (ch->count + required >= new_cap) {
            new_cap *= GROWTH_FACTOR;
        }
        int32_t *new_ptr = (int32_t*)realloc(ch->samples, new_cap * sizeof(int32_t));
        if (!new_ptr) {
            fprintf(stderr, "Error reallocating channel\n");
            exit(1);
        }
        ch->samples = new_ptr;
        ch->capacity = new_cap;
    }
}

// ============================================================================
// LEITURA DE ÁUDIO (MODIFICADO PARA 32 BITS)
// ============================================================================

int precisa_converter(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 1;
    // Força conversão se não for WAV ou se quisermos garantir o formato correto
    return 1; 
}

int convert_to_wav_temp(const char *audio_file, char *temp_wav) {
    sprintf(temp_wav, "temp_txac_%d.wav", (int)time(NULL));
    const char *ext = strrchr(audio_file, '.');
    const char *formato = ext ? ext + 1 : "áudio";
    
    char cmd[2048];
    // ALTERAÇÃO: Mudamos pcm_s16le para pcm_s32le (32-bit Signed Integer)
    sprintf(cmd, "ffmpeg -i \"%s\" -f wav -acodec pcm_s32le -rf64 never \"%s\" -y -loglevel error", 
            audio_file, temp_wav);
    
    printf("Converting %s to WAV 32-bit...\n", formato);
    if (system(cmd) != 0) {
        fprintf(stderr, "Error converting FFmpeg\n");
        return 0;
    }
    return 1;
}

int ler_wav_multicanal(const char *arquivo, Channel channels[], TXACHeader *header) {
    FILE *f = fopen(arquivo, "rb");
    if (!f) {
        perror("Error opening WAV");
        return 0;
    }

    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44) {
        fclose(f);
        return 0;
    }

    memcpy(&header->sample_rate, hdr + 24, 4);
    memcpy(&header->channels, hdr + 22, 2);
    memcpy(&header->bits_per_sample, hdr + 34, 2);

    printf("WAV Info: %d Hz, %d canais, %d bits\n", 
           header->sample_rate, header->channels, header->bits_per_sample);

    // Procura chunk 'data'
    fseek(f, 12, SEEK_SET);
    uint8_t chunk_id[4];
    uint32_t chunk_size;
    int found = 0;
    
    printf("Searching for chunk 'data'...\n");
    
    while (fread(chunk_id, 1, 4, f) == 4) {
        fread(&chunk_size, 4, 1, f);
        
        if (memcmp(chunk_id, "data", 4) == 0) {
            printf("Chunk 'data' found (%u bytes)\n", chunk_size);
            found = 1;
            break;
        }
        
        if (chunk_size > 0 && chunk_size < 0x7FFFFFFF) {
            fseek(f, chunk_size, SEEK_CUR);
        } else break;
    }
    
    if (!found) {
        fprintf(stderr, "Chunk 'data' not found\n");
        fclose(f);
        return 0;
    }

    // Inicializa canais
    for (int i = 0; i < header->channels; i++) {
        init_channel(&channels[i], 512 * 1024);
    }

    // Fator de redução
    float fator_f = (float)pow(10.0, -DB_REDUCTION / 20.0);

    // =========================================================
    // LÓGICA DE LEITURA 32-BIT
    // =========================================================
    if (header->bits_per_sample == 32) {
        int32_t buffer[8 * MAX_CHANNELS]; // Buffer para ler blocos
        size_t num_read;
        
        while ((num_read = fread(buffer, sizeof(int32_t), 8 * header->channels, f)) > 0) {
            // Se leu um número quebrado de samples (fim do arquivo), ajusta loop
            size_t total_samples = num_read; 
            
            for (size_t i = 0; i < total_samples; i++) {
                int ch = i % header->channels;
                int32_t s32 = buffer[i];
                
                // Aplica redução de volume diretamente no int32
                int32_t reduced = (int32_t)(s32 * fator_f);
                
                ensure_channel_capacity(&channels[ch], 1);
                channels[ch].samples[channels[ch].count++] = reduced;
            }
        }
    }
    // =========================================================
    // FALLBACK PARA 16-BIT (Caso o usuário forneça um WAV antigo)
    // =========================================================
    else if (header->bits_per_sample == 16) {
        int16_t buffer[8 * MAX_CHANNELS];
        size_t num_read;
        
        while ((num_read = fread(buffer, sizeof(int16_t), 8 * header->channels, f)) > 0) {
            for (size_t i = 0; i < num_read; i++) {
                int ch = i % header->channels;
                int16_t s16 = buffer[i];
                // Converte 16 -> 32 bit movendo para a parte alta
                int32_t s32 = ((int32_t)s16) << 16;
                int32_t reduced = (int32_t)(s32 * fator_f);
                
                ensure_channel_capacity(&channels[ch], 1);
                channels[ch].samples[channels[ch].count++] = reduced;
            }
        }
    } else {
        fprintf(stderr, "Error: Only 16-bit and 32-bit surported. File is %d-bit.\n", header->bits_per_sample);
        fclose(f);
        return 0;
    }

    fclose(f);
    header->total_samples = channels[0].count;
    printf("Done reading: %zu samples per channel\n", channels[0].count);
    return 1;
}

// ============================================================================
// SCANNER DE LOOP (COM AVX2)
// ============================================================================

typedef struct {
    int64_t start_pos;
    int64_t loop_size;
    int is_consecutive; 
} LoopInfo;

LoopInfo detectar_loop_robusto(Channel *ch) {
    LoopInfo info = {-1, 0, 0};
    if (ch->count < 44100) return info; 
    
    printf(" Analysing %zu samples with Scanner AVX2...\n", ch->count);

    size_t signature_len = 15000;
    if (signature_len > ch->count / 2) signature_len = ch->count / 10;
    
    size_t end_idx = ch->count - signature_len;
    int32_t *signature = &ch->samples[end_idx];

    size_t found_pos = 0;
    int found = 0;

    for (size_t i = end_idx - signature_len; i > signature_len; i -= 10) { 
        if (ch->samples[i] == signature[0]) {
            int match = 1;
            size_t j = 1;

            // --- OTIMIZAÇÃO AVX2 ---
            while (j + 8 <= signature_len) {
                __m256i v_audio = _mm256_loadu_si256((__m256i*)&ch->samples[i + j]);
                __m256i v_sig   = _mm256_loadu_si256((__m256i*)&signature[j]);
                __m256i v_cmp = _mm256_cmpeq_epi32(v_audio, v_sig);
                int mask = _mm256_movemask_ps(_mm256_castsi256_ps(v_cmp));

                if (mask != 0xFF) {
                    match = 0;
                    break;
                }
                j += 8;
            }

            if (match) {
                for (; j < signature_len; j++) {
                    if (ch->samples[i + j] != signature[j]) {
                        match = 0;
                        break;
                    }
                }
            }
            
            if (match) {
                found_pos = i;
                found = 1;
                break;
            }
        }
    }

    if (found) {
        size_t loop_len = signature_len;
        size_t cursor_a = found_pos;
        size_t cursor_b = end_idx;
        
        while (cursor_a > 0 && cursor_b > found_pos) { 
            if (ch->samples[cursor_a - 1] == ch->samples[cursor_b - 1]) {
                cursor_a--;
                cursor_b--;
                loop_len++;
            } else {
                break; 
            }
        }
        
        printf("Loop detectected! Start: %zu, Size: %zu\n", cursor_b, loop_len);
        
        info.start_pos = cursor_b;
        info.loop_size = loop_len;
        
        if (cursor_b + loop_len >= ch->count - 100) { 
            info.is_consecutive = 1; 
        } else {
            info.is_consecutive = 0;
        }
        return info;
    }

    printf("No obvious loops found.\n");
    return info;
}

// ============================================================================
// COMPRESSÃO COM 4-BIT DIRETO
// ============================================================================

void *compactar_canal_4bit_thread(void *arg) {
    ThreadData *td = (ThreadData*)arg;
    Channel *ch = td->channel;
    Binary4BitBuffer *out = td->output;
    
    init_4bit_buffer(out);
    
    printf("  [Channel %d] Direct compression to 4 bits...\n", td->channel_id);
    
    LoopInfo loop_info = {-1, 0, 0};
    if (td->enable_loop_compression) {
        loop_info = detectar_loop_robusto(ch);
    }
    
    size_t end_pos = (loop_info.start_pos > 0) ? loop_info.start_pos : ch->count;
    char temp[128];
    size_t i = 0;
    
    while (i < end_pos) {
        int32_t atual = ch->samples[i];
        
        // 1. Tenta ^
        size_t count = 1;
        while (i + count < end_pos && ch->samples[i + count] == atual) {
            count++;
        }
        
        if (count >= 2) {
            sprintf(temp, "%d^%zu,", atual, count);
            write_string_4bit(out, temp);
            i += count;
            continue;
        }
        
        // 2. Tenta ~
        int sniper_found = 0;
        for (int dist = 2; dist <= 99 && i + dist < end_pos; dist++) {
            if (ch->samples[i + dist] == atual) {
                sprintf(temp, "%d~%d,", atual, dist - 1);
                write_string_4bit(out, temp);
                
                for (int j = i + 1; j < i + dist; j++) {
                    sprintf(temp, "%d,", ch->samples[j]);
                    write_string_4bit(out, temp);
                }
                
                i = i + dist + 1;
                sniper_found = 1;
                break;
            }
        }
        
        if (!sniper_found) {
            sprintf(temp, "%d,", atual);
            write_string_4bit(out, temp);
            i++;
        }
    }
    
    if (loop_info.start_pos > 0) {
        if (loop_info.is_consecutive) {
            sprintf(temp, "LOOP^%lld,", loop_info.loop_size);
            write_string_4bit(out, temp);
        } else {
            sprintf(temp, "LOOP~%lld,", loop_info.start_pos);
            write_string_4bit(out, temp);
        }
    }
    
    finalize_4bit_buffer(out);

     printf("  [Channel %d] Compressed: %zu bytes (4-bit)\n", td->channel_id, out->byte_count);

    return NULL;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Uso: %s <input> <output.txac> [--loop]\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *output = argv[2];
    int enable_loop = (argc >= 4 && strcmp(argv[3], "--loop") == 0);

    printf("\n=== TXAC input v0.2.0 ===\n");

    char temp_wav[256] = {0};
    int is_temp = 0;

    // Sempre tenta converter para garantir 32-bit se não for um WAV confiável
    if (precisa_converter(input)) {
        if (!convert_to_wav_temp(input, temp_wav)) return 1;
        input = temp_wav;
        is_temp = 1;
    }

    Channel channels[MAX_CHANNELS];
    TXACHeader header = {0};
    
    if (!ler_wav_multicanal(input, channels, &header)) {
        if (is_temp) remove(temp_wav);
        return 1;
    }

    printf("\n🔄 Compactando %d canais...\n", header.channels);
    
    pthread_t threads[MAX_CHANNELS];
    ThreadData thread_data[MAX_CHANNELS];
    Binary4BitBuffer outputs[MAX_CHANNELS];
    
    for (int i = 0; i < header.channels; i++) {
        thread_data[i].channel = &channels[i];
        thread_data[i].output = &outputs[i];
        thread_data[i].channel_id = i;
        thread_data[i].enable_loop_compression = enable_loop;
        
        pthread_create(&threads[i], NULL, compactar_canal_4bit_thread, &thread_data[i]);
    }
    
    for (int i = 0; i < header.channels; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("\n💾 Salvando container TXAC v3...\n");
    FILE *fout = fopen(output, "wb");
    if (!fout) {
        perror("Erro ao criar arquivo");
        return 1;
    }

    fwrite(TXAC_MAGIC, 1, 4, fout);
    uint32_t version = TXAC_VERSION;
    fwrite(&version, 4, 1, fout);
    fwrite(&header.sample_rate, 4, 1, fout);
    fwrite(&header.channels, 2, 1, fout);
    
    // Agora salvamos como 32 bits no cabeçalho
    uint16_t save_bits = 32; 
    fwrite(&save_bits, 2, 1, fout);
    
    uint32_t flags = enable_loop ? 1 : 0;
    fwrite(&flags, 4, 1, fout);
    fwrite(&header.total_samples, 8, 1, fout);
    
    uint8_t reserved[36] = {0};
    fwrite(reserved, 1, 36, fout);

    long index_pos = ftell(fout);
    uint64_t offsets[MAX_CHANNELS] = {0};
    uint64_t sizes[MAX_CHANNELS] = {0};
    
    for (int i = 0; i < header.channels; i++) {
        fwrite(&offsets[i], 8, 1, fout);
        fwrite(&sizes[i], 8, 1, fout);
    }

    for (int i = 0; i < header.channels; i++) {
        offsets[i] = ftell(fout);
        fwrite(outputs[i].data, 1, outputs[i].byte_count, fout);
        sizes[i] = outputs[i].byte_count;
    }

    fseek(fout, index_pos, SEEK_SET);
    for (int i = 0; i < header.channels; i++) {
        fwrite(&offsets[i], 8, 1, fout);
        fwrite(&sizes[i], 8, 1, fout);
    }

    // Dados dos canais (já em 4-bit!)
    for (int i = 0; i < header.channels; i++) {
        offsets[i] = ftell(fout);
        fwrite(outputs[i].data, 1, outputs[i].byte_count, fout);
        sizes[i] = outputs[i].byte_count;
        printf("  Canal %d: %llu bytes\n", i, sizes[i]);
    }


    fclose(fout);

    for (int i = 0; i < header.channels; i++) {
        free(channels[i].samples);
        free(outputs[i].data);
    }
    if (is_temp) remove(temp_wav);

    printf("\n✅ Codificação concluída!\n");
    return 0;
}