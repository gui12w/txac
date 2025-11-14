/*
TXAC Decoder - Converte .txac para WAV
Tudo processado em RAM com otimização AVX

Compilar:
    zig cc avx_output_v2.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_decode.exe
    
Uso:
    txac_decode input.txac output.wav sample_rate channels
    txac_decode audio.txac audio.wav 44100 2
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>

#define INITIAL_CHARS_CAPACITY (1024 * 1024)
#define INITIAL_SAMPLES_CAPACITY (512 * 1024)
#define GROWTH_FACTOR 2
#define GAIN_DB 110.0f
#define AMPLITUDE_FACTOR pow(10.0f, GAIN_DB / 20.0f)
#define INT32_MAX_VAL 2147483647
#define INT32_MIN_VAL -2147483648
#define AVX_BLOCK_SIZE 8

// Mapa de símbolos
const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

// Estruturas
typedef struct {
    char *data;
    size_t capacity;
    size_t length;
} TextBuffer;

typedef struct {
    int32_t *samples;
    size_t capacity;
    int sample_count;
} AudioBuffer;

char bit4_para_char(int val) {
    if (val >= 0 && val < 16)
        return simbolos[val];
    return '?';
}

// ============================================================================
// BUFFER DE TEXTO
// ============================================================================

void init_text_buffer(TextBuffer *buf) {
    buf->capacity = INITIAL_CHARS_CAPACITY;
    buf->length = 0;
    buf->data = (char*)malloc(buf->capacity);
    if (!buf->data) {
        fprintf(stderr, "Erro ao alocar buffer de texto\n");
        exit(1);
    }
}

void ensure_text_capacity(TextBuffer *buf, size_t required) {
    if (buf->length + required >= buf->capacity) {
        size_t new_cap = buf->capacity * GROWTH_FACTOR;
        while (buf->length + required >= new_cap) {
            new_cap *= GROWTH_FACTOR;
        }
        char *new_ptr = (char*)realloc(buf->data, new_cap);
        if (!new_ptr) {
            fprintf(stderr, "Erro ao realocar texto\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

// ============================================================================
// BUFFER DE ÁUDIO
// ============================================================================

void init_audio_buffer(AudioBuffer *buf) {
    buf->capacity = INITIAL_SAMPLES_CAPACITY;
    buf->sample_count = 0;
    buf->samples = (int32_t*)malloc(buf->capacity * sizeof(int32_t));
    if (!buf->samples) {
        fprintf(stderr, "Erro ao alocar buffer de áudio\n");
        exit(1);
    }
}

void ensure_audio_capacity(AudioBuffer *buf, size_t required) {
    if (buf->sample_count + required >= buf->capacity) {
        size_t new_cap = buf->capacity * GROWTH_FACTOR;
        while (buf->sample_count + required >= new_cap) {
            new_cap *= GROWTH_FACTOR;
        }
        int32_t *new_ptr = (int32_t*)realloc(buf->samples, new_cap * sizeof(int32_t));
        if (!new_ptr) {
            fprintf(stderr, "Erro ao realocar áudio\n");
            exit(1);
        }
        buf->samples = new_ptr;
        buf->capacity = new_cap;
    }
}

// ============================================================================
// DECODIFICAÇÃO BINÁRIA
// ============================================================================

void binario_para_texto(const char *arquivo_bin, TextBuffer *buf) {
    FILE *f = fopen(arquivo_bin, "rb");
    if (!f) {
        perror("Erro ao abrir arquivo TXAC");
        exit(1);
    }

    init_text_buffer(buf);
    int byte;

    while ((byte = fgetc(f)) != EOF) {
        ensure_text_capacity(buf, 2);
        int high = (byte >> 4) & 0x0F;
        int low = byte & 0x0F;
        buf->data[buf->length++] = bit4_para_char(high);
        buf->data[buf->length++] = bit4_para_char(low);
    }

    ensure_text_capacity(buf, 1);
    buf->data[buf->length] = '\0';
    fclose(f);
    
    printf("✅ Arquivo binário decodificado: %zu bytes\n", buf->length);
}

// ============================================================================
// DESCOMPACTAÇÃO COM AVX
// ============================================================================

void descompactar_string(TextBuffer *text, AudioBuffer *audio) {
    init_audio_buffer(audio);
    
    char buffer[128];
    size_t i = 0;
    size_t len = text->length;

    // Vetores AVX para aplicar ganho
    float factor_float = (float)AMPLITUDE_FACTOR;
    __m256 factor_vec = _mm256_set1_ps(factor_float);
    __m256i max_vec = _mm256_set1_epi32(INT32_MAX_VAL);
    __m256i min_vec = _mm256_set1_epi32(INT32_MIN_VAL);

    printf("🔄 Descompactando e aplicando ganho de %.1f dB...\n", GAIN_DB);

    while (i < len) {
        // Parse próximo token
        int b = 0;
        while (i < len && text->data[i] != ',') {
            if (b < sizeof(buffer) - 1) {
                buffer[b++] = text->data[i++];
            } else {
                i++;
            }
        }
        buffer[b] = '\0';
        i++;

        double num_double;
        int rep;

        // Padrão: número^repetições (usa AVX)
        if (sscanf(buffer, "%lf^%d", &num_double, &rep) == 2) {
            ensure_audio_capacity(audio, rep);
            
            __m256 sample_vec = _mm256_set1_ps((float)num_double);
            int avx_reps = rep / AVX_BLOCK_SIZE;

            // Processamento AVX
            for (int r = 0; r < avx_reps; r++) {
                __m256 boosted = _mm256_mul_ps(sample_vec, factor_vec);
                __m256i boosted_int = _mm256_cvttps_epi32(boosted);
                __m256i clipped_min = _mm256_max_epi32(min_vec, boosted_int);
                __m256i clipped = _mm256_min_epi32(max_vec, clipped_min);
                _mm256_storeu_si256((__m256i*)&audio->samples[audio->sample_count], clipped);
                audio->sample_count += AVX_BLOCK_SIZE;
            }

            // Cauda
            int remaining = rep % AVX_BLOCK_SIZE;
            double boosted_seq = num_double * AMPLITUDE_FACTOR;
            for (int r = 0; r < remaining; r++) {
                if (boosted_seq > INT32_MAX_VAL) 
                    audio->samples[audio->sample_count++] = INT32_MAX_VAL;
                else if (boosted_seq < INT32_MIN_VAL) 
                    audio->samples[audio->sample_count++] = INT32_MIN_VAL;
                else 
                    audio->samples[audio->sample_count++] = (int32_t)boosted_seq;
            }

        } 
        // Padrão: número~seguido (sem AVX)
        else if (sscanf(buffer, "%lf~%d", &num_double, &rep) == 2) {
            ensure_audio_capacity(audio, rep + 2);
            double boosted = num_double * AMPLITUDE_FACTOR;
            
            int32_t val;
            if (boosted > INT32_MAX_VAL) val = INT32_MAX_VAL;
            else if (boosted < INT32_MIN_VAL) val = INT32_MIN_VAL;
            else val = (int32_t)boosted;
            
            audio->samples[audio->sample_count++] = val;

            for (int r = 0; r < rep; r++) {
                b = 0;
                while (i < len && text->data[i] != ',') {
                    if (b < sizeof(buffer) - 1) {
                        buffer[b++] = text->data[i++];
                    } else {
                        i++;
                    }
                }
                buffer[b] = '\0';
                i++;

                double temp;
                if (sscanf(buffer, "%lf", &temp) == 1) {
                    ensure_audio_capacity(audio, 1);
                    double temp_boosted = temp * AMPLITUDE_FACTOR;
                    
                    int32_t temp_val;
                    if (temp_boosted > INT32_MAX_VAL) temp_val = INT32_MAX_VAL;
                    else if (temp_boosted < INT32_MIN_VAL) temp_val = INT32_MIN_VAL;
                    else temp_val = (int32_t)temp_boosted;
                    
                    audio->samples[audio->sample_count++] = temp_val;
                }
            }
            
            audio->samples[audio->sample_count++] = val;

        } 
        // Padrão: apenas número
        else if (sscanf(buffer, "%lf", &num_double) == 1) {
            ensure_audio_capacity(audio, 1);
            double boosted = num_double * AMPLITUDE_FACTOR;
            
            int32_t val;
            if (boosted > INT32_MAX_VAL) val = INT32_MAX_VAL;
            else if (boosted < INT32_MIN_VAL) val = INT32_MIN_VAL;
            else val = (int32_t)boosted;
            
            audio->samples[audio->sample_count++] = val;
        }
    }

    printf("✅ Descompactado: %d amostras\n", audio->sample_count);
}

// ============================================================================
// SALVAR WAV
// ============================================================================

void salvar_wav(const char *arquivo, AudioBuffer *audio, uint32_t sample_rate, uint16_t channels) {
    FILE *f = fopen(arquivo, "wb");
    if (!f) {
        perror("Erro ao criar WAV");
        return;
    }

    printf("💾 Salvando WAV: %d Hz, %d canais...\n", sample_rate, channels);

    uint16_t bits_per_sample = 32;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    // Cabeçalho WAV
    uint8_t header[44];
    memset(header, 0, sizeof(header));

    memcpy(header, "RIFF", 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    
    uint32_t subchunk1_size = 16;
    memcpy(header + 16, &subchunk1_size, 4);
    
    uint16_t audio_format = 1;
    memcpy(header + 20, &audio_format, 2);
    memcpy(header + 22, &channels, 2);
    memcpy(header + 24, &sample_rate, 4);
    memcpy(header + 28, &byte_rate, 4);
    memcpy(header + 32, &block_align, 2);
    memcpy(header + 34, &bits_per_sample, 2);
    memcpy(header + 36, "data", 4);

    fwrite(header, 1, 44, f);

    // Escreve amostras
    for (int i = 0; i < audio->sample_count; i++) {
        fwrite(&audio->samples[i], sizeof(int32_t), 1, f);
    }

    // Atualiza tamanhos
    int32_t data_size = audio->sample_count * sizeof(int32_t);
    int32_t file_size = 36 + data_size;

    fseek(f, 4, SEEK_SET);
    fwrite(&file_size, 4, 1, f);

    fseek(f, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, f);

    fclose(f);
    printf("✅ WAV salvo: %s (%d amostras)\n", arquivo, audio->sample_count);
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 5) {
        printf("Uso: %s <input.txac> <output.wav> <sample_rate> <channels>\n", argv[0]);
        printf("Exemplo: %s audio.txac audio.wav 44100 2\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *output = argv[2];
    uint32_t sample_rate = atoi(argv[3]);
    uint16_t channels = atoi(argv[4]);

    printf("\n=== TXAC DECODER ===\n");
    printf("Input: %s\n", input);
    printf("Output: %s\n", output);
    printf("Sample rate: %d Hz\n", sample_rate);
    printf("Canais: %d\n\n", channels);

    TextBuffer text = {0};
    AudioBuffer audio = {0};

    binario_para_texto(input, &text);
    descompactar_string(&text, &audio);
    salvar_wav(output, &audio, sample_rate, channels);

    free(text.data);
    free(audio.samples);

    printf("\n✅ Decodificação concluída!\n");
    return 0;
}