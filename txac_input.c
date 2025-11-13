/*
TXAC Encoder - Converte áudio para .txac
Suporta: WAV (16/32-bit), FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, etc.
Qualquer formato suportado pelo ffmpeg será convertido automaticamente
Tudo processado em RAM, sem arquivos intermediários
Redução fixa: 110 dB

Compilar:
    zig cc avx_input_v2.c -std=gnu99 -pthread -O3 -mavx2 -lm -o txac_encode.exe
    
Uso:
    txac_encode input.wav output.txac
    txac_encode input.flac output.txac
    txac_encode input.mp3 output.txac
    txac_encode input.m4a output.txac
    txac_encode input.ogg output.txac
    ... (qualquer formato de áudio)
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>      // Para time()
#include <immintrin.h> // Para AVX

#define INITIAL_CAPACITY 1024
#define AVX_BLOCK_SIZE 8
#define GROWTH_FACTOR 2

// Estrutura para armazenar dados WAV em memória
typedef struct {
    int32_t *samples;
    size_t sample_count;
    size_t capacity;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
} AudioBuffer;

// Estrutura para texto compactado
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} TextBuffer;

// Mapa de 16 símbolos válidos
const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

// ============================================================================
// FUNÇÕES DE BUFFER
// ============================================================================

void init_audio_buffer(AudioBuffer *buf) {
    buf->capacity = INITIAL_CAPACITY;
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
            fprintf(stderr, "Erro ao realocar buffer de áudio\n");
            exit(1);
        }
        buf->samples = new_ptr;
        buf->capacity = new_cap;
    }
}

void init_text_buffer(TextBuffer *buf) {
    buf->capacity = 1024 * 1024; // 1 MB inicial
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
            fprintf(stderr, "Erro ao realocar buffer de texto\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

void append_text(TextBuffer *buf, const char *str) {
    size_t len = strlen(str);
    ensure_text_capacity(buf, len + 1);
    memcpy(buf->data + buf->length, str, len);
    buf->length += len;
    buf->data[buf->length] = '\0';
}

// ============================================================================
// LEITURA DE ARQUIVOS DE ÁUDIO
// ============================================================================

// Verifica se o arquivo NÃO é WAV (para converter)
int precisa_converter(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return 1; // Sem extensão, tenta converter
    
    // Apenas .wav não precisa converter
    return !(strcmp(ext, ".wav") == 0 || strcmp(ext, ".WAV") == 0);
}

// Converte qualquer formato de áudio para WAV usando ffmpeg
int convert_audio_to_wav_temp(const char *audio_file, char *temp_wav) {
    sprintf(temp_wav, "temp_txac_%d.wav", (int)time(NULL));
    
    // Detecta extensão para mensagem
    const char *ext = strrchr(audio_file, '.');
    const char *formato = ext ? ext + 1 : "áudio";
    
    char cmd[1024];
    // Converte qualquer formato suportado pelo ffmpeg para WAV 16-bit
    sprintf(cmd, "ffmpeg -i \"%s\" -f wav -acodec pcm_s16le \"%s\" -y -loglevel error", 
            audio_file, temp_wav);
    
    printf("🔄 Convertendo %s para WAV temporário (16-bit)...\n", formato);
    int result = system(cmd);
    
    if (result != 0) {
        fprintf(stderr, "❌ Erro: ffmpeg não encontrado ou formato não suportado.\n");
        fprintf(stderr, "Instale o ffmpeg para converter outros formatos de áudio.\n");
        fprintf(stderr, "Formatos suportados: FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, etc.\n");
        return 0;
    }
    printf("✅ Conversão de %s concluída\n", formato);
    return 1;
}

// Lê arquivo WAV (16-bit ou 32-bit) para buffer
int ler_wav(const char *arquivo_wav, AudioBuffer *buf, double reduzir_db) {
    FILE *f = fopen(arquivo_wav, "rb");
    if (!f) {
        perror("Erro ao abrir WAV");
        return 0;
    }

    // Lê cabeçalho WAV
    uint8_t header[44];
    if (fread(header, 1, 44, f) != 44) {
        fprintf(stderr, "Arquivo WAV inválido\n");
        fclose(f);
        return 0;
    }

    // Extrai informações do header
    memcpy(&buf->sample_rate, header + 24, 4);
    memcpy(&buf->channels, header + 22, 2);
    memcpy(&buf->bits_per_sample, header + 34, 2);

    printf("📊 WAV Info: %d Hz, %d canais, %d bits\n", 
           buf->sample_rate, buf->channels, buf->bits_per_sample);

    // Calcula fator de redução AVX
    float fator_f = (float)pow(10.0, -reduzir_db / 20.0);
    __m256 factor_vec = _mm256_set1_ps(fator_f);

    init_audio_buffer(buf);

    // Variáveis para detectar e prevenir clipping
    int64_t sample_sum = 0;
    int64_t sample_max = 0;
    size_t samples_read = 0;

    if (buf->bits_per_sample == 32) {
        // ===== WAV 32-BIT (com AVX) =====
        int32_t samples_in[AVX_BLOCK_SIZE];
        int32_t samples_out[AVX_BLOCK_SIZE];
        size_t num_read;

        while ((num_read = fread(samples_in, sizeof(int32_t), AVX_BLOCK_SIZE, f)) == AVX_BLOCK_SIZE) {
            ensure_audio_capacity(buf, AVX_BLOCK_SIZE);

            __m256i int_vec = _mm256_loadu_si256((__m256i*)samples_in);
            __m256 float_vec = _mm256_cvtepi32_ps(int_vec);
            __m256 reduced_vec = _mm256_mul_ps(float_vec, factor_vec);
            __m256i reduced_int = _mm256_cvttps_epi32(reduced_vec);
            _mm256_storeu_si256((__m256i*)samples_out, reduced_int);

            for (int i = 0; i < AVX_BLOCK_SIZE; i++) {
                int32_t s = samples_out[i];
                buf->samples[buf->sample_count++] = s;
                
                // Estatísticas para debug
                int64_t abs_val = (s < 0) ? -(int64_t)s : (int64_t)s;
                sample_sum += abs_val;
                if (abs_val > sample_max) sample_max = abs_val;
                samples_read++;
            }
        }

        // Cauda
        for (size_t i = 0; i < num_read; i++) {
            ensure_audio_capacity(buf, 1);
            int32_t s = (int32_t)(samples_in[i] * fator_f);
            buf->samples[buf->sample_count++] = s;
            
            int64_t abs_val = (s < 0) ? -(int64_t)s : (int64_t)s;
            sample_sum += abs_val;
            if (abs_val > sample_max) sample_max = abs_val;
            samples_read++;
        }

    } else if (buf->bits_per_sample == 16) {
        // ===== WAV 16-BIT (converte para 32-bit com AVX) =====
        printf("🔄 Convertendo 16-bit para 32-bit...\n");
        
        int16_t samples_16[AVX_BLOCK_SIZE];
        int32_t samples_32[AVX_BLOCK_SIZE];
        size_t num_read;

        while ((num_read = fread(samples_16, sizeof(int16_t), AVX_BLOCK_SIZE, f)) == AVX_BLOCK_SIZE) {
            ensure_audio_capacity(buf, AVX_BLOCK_SIZE);

            // Converte 16-bit para 32-bit (shift 16 bits)
            for (int i = 0; i < AVX_BLOCK_SIZE; i++) {
                samples_32[i] = (int32_t)samples_16[i] << 16;
            }

            // Aplica redução com AVX
            __m256i int_vec = _mm256_loadu_si256((__m256i*)samples_32);
            __m256 float_vec = _mm256_cvtepi32_ps(int_vec);
            __m256 reduced_vec = _mm256_mul_ps(float_vec, factor_vec);
            __m256i reduced_int = _mm256_cvttps_epi32(reduced_vec);
            
            int32_t output[AVX_BLOCK_SIZE];
            _mm256_storeu_si256((__m256i*)output, reduced_int);

            for (int i = 0; i < AVX_BLOCK_SIZE; i++) {
                int32_t s = output[i];
                buf->samples[buf->sample_count++] = s;
                
                int64_t abs_val = (s < 0) ? -(int64_t)s : (int64_t)s;
                sample_sum += abs_val;
                if (abs_val > sample_max) sample_max = abs_val;
                samples_read++;
            }
        }

        // Cauda
        for (size_t i = 0; i < num_read; i++) {
            ensure_audio_capacity(buf, 1);
            int32_t sample_32 = ((int32_t)samples_16[i]) << 16;
            int32_t s = (int32_t)(sample_32 * fator_f);
            buf->samples[buf->sample_count++] = s;
            
            int64_t abs_val = (s < 0) ? -(int64_t)s : (int64_t)s;
            sample_sum += abs_val;
            if (abs_val > sample_max) sample_max = abs_val;
            samples_read++;
        }

    } else {
        fprintf(stderr, "Erro: Apenas WAV 16-bit e 32-bit são suportados\n");
        fclose(f);
        return 0;
    }

    fclose(f);
    
    // Calcula média de amplitude
    int64_t avg_amplitude = samples_read > 0 ? sample_sum / samples_read : 0;
    
    printf("✅ %zu amostras carregadas\n", buf->sample_count);
    printf("📊 Amplitude máxima: %lld (%.1f%%)\n", 
           (long long)sample_max, 
           (sample_max * 100.0) / 2147483647.0);
    printf("📊 Amplitude média: %lld (%.1f%%)\n", 
           (long long)avg_amplitude,
           (avg_amplitude * 100.0) / 2147483647.0);
    
    // Aviso se detectar possível clipping
    if (sample_max > 2000000000) { // ~93% do range
        printf("⚠️  AVISO: Amplitude muito alta detectada!\n");
        printf("   O áudio pode ter clipping após aplicar ganho de 110 dB.\n");
    }
    
    return 1;
}

// ============================================================================
// COMPACTAÇÃO
// ============================================================================

void compactar_para_texto(AudioBuffer *audio, TextBuffer *text) {
    init_text_buffer(text);
    
    char temp[128];
    size_t i = 0;
    size_t n = audio->sample_count;

    printf("🗜️  Compactando...\n");

    while (i < n) {
        int32_t atual = audio->samples[i];

        // Tenta compressão ^ (repetição)
        size_t count = 1;
        while (i + count < n && audio->samples[i + count] == atual) {
            count++;
        }

        if (count >= 2) {
            sprintf(temp, "%d^%zu,", atual, count);
            append_text(text, temp);
            i += count;
            continue;
        }

        // Tenta compressão ~ (sniper)
        int sniper_encontrado = 0;
        for (int dist = 2; dist <= 99 && i + dist < n; dist++) {
            if (audio->samples[i + dist] == atual) {
                sprintf(temp, "%d~%d,", atual, dist - 1);
                append_text(text, temp);

                for (int j = i + 1; j < i + dist; j++) {
                    sprintf(temp, "%d,", audio->samples[j]);
                    append_text(text, temp);
                }

                i = i + dist + 1;
                sniper_encontrado = 1;
                break;
            }
        }

        if (!sniper_encontrado) {
            sprintf(temp, "%d,", atual);
            append_text(text, temp);
            i++;
        }
    }

    printf("✅ Texto compactado: %zu bytes\n", text->length);
}

// ============================================================================
// CONVERSÃO 4-BIT
// ============================================================================

int char_para_4bit(char c) {
    for (int i = 0; i < 16; i++) {
        if (simbolos[i] == c) return i;
    }
    return -1;
}

void texto_para_4bits(TextBuffer *text, const char *arquivo_saida) {
    FILE *fout = fopen(arquivo_saida, "wb");
    if (!fout) {
        perror("Erro ao criar arquivo de saída");
        exit(1);
    }

    printf("📦 Convertendo para 4-bit binário...\n");

    int high = -1;
    for (size_t i = 0; i < text->length; i++) {
        int nibble = char_para_4bit(text->data[i]);
        if (nibble == -1) continue;

        if (high == -1) {
            high = nibble;
        } else {
            unsigned char byte = (high << 4) | nibble;
            fwrite(&byte, 1, 1, fout);
            high = -1;
        }
    }

    if (high != -1) {
        unsigned char byte = (high << 4);
        fwrite(&byte, 1, 1, fout);
    }

    fclose(fout);
    printf("✅ Arquivo TXAC salvo: %s\n", arquivo_saida);
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Uso: %s <input_audio> <output.txac>\n", argv[0]);
        printf("\nFormatos suportados:\n");
        printf("  - WAV (nativo, 16-bit ou 32-bit)\n");
        printf("  - FLAC, MP3, AAC, M4A, OGG, OPUS, WMA, etc. (via ffmpeg)\n");
        printf("\nExemplos:\n");
        printf("  %s audio.wav audio.txac\n", argv[0]);
        printf("  %s audio.flac audio.txac\n", argv[0]);
        printf("  %s audio.mp3 audio.txac\n", argv[0]);
        printf("  %s audio.m4a audio.txac\n", argv[0]);
        printf("\nRedução fixa: 110 dB\n");
        return 1;
    }

    const char *input = argv[1];
    const char *output = argv[2];
    const double db_reduction = 110.0; // FIXO

    printf("\n=== TXAC ENCODER ===\n");
    printf("Input: %s\n", input);
    printf("Output: %s\n", output);
    printf("Redução: %.1f dB (fixo)\n\n", db_reduction);

    char temp_wav[256] = {0};
    int is_temp = 0;

    // Se NÃO for WAV, converte usando ffmpeg
    if (precisa_converter(input)) {
        if (!convert_audio_to_wav_temp(input, temp_wav)) {
            return 1;
        }
        input = temp_wav;
        is_temp = 1;
    } else {
        printf("📄 Arquivo WAV detectado, processando diretamente...\n");
    }

    AudioBuffer audio = {0};
    TextBuffer text = {0};

    // Pipeline completo em RAM
    if (!ler_wav(input, &audio, db_reduction)) {
        if (is_temp) remove(temp_wav);
        return 1;
    }

    compactar_para_texto(&audio, &text);
    texto_para_4bits(&text, output);

    // Limpeza
    free(audio.samples);
    free(text.data);
    if (is_temp) remove(temp_wav);

    printf("\n✅ Conversão concluída!\n");
    return 0;
}