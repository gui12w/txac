/*
TXAC Player - Player de áudio para arquivos .txac
Baseado no qoaplay.c

Requer:
    - "sokol_audio.h" (https://github.com/floooh/sokol/blob/master/sokol_audio.h)

Compilar com Zig:
    zig cc txacplay.c -std=gnu99 -lasound -pthread -O3 -o txacplay

Ou com GCC:
    gcc txacplay.c -std=gnu99 -lasound -pthread -O3 -o txacplay -lm
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define SOKOL_AUDIO_IMPL
#include "sokol_audio.h"

/* ============================================================================
   DECODER TXAC - Adaptado do seu codec
   ============================================================================ */

#define INITIAL_CHARS_CAPACITY (1024 * 1024)  // 1 MB inicial
#define INITIAL_SAMPLES_CAPACITY (512 * 1024) // 512 KB inicial
#define GROWTH_FACTOR 2
#define GAIN_DB 110.0f
#define AMPLITUDE_FACTOR pow(10.0f, GAIN_DB / 20.0f)
#define INT32_MAX_VAL 2147483647
#define INT32_MIN_VAL -2147483648
#define FRAME_SIZE 2048  // Número de amostras por frame
#define AVX_BLOCK_SIZE 8 // Processa 8 floats por vez com AVX

// Mapa de 16 símbolos válidos
const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

// Converte valor de 4 bits para caractere
char bit4_para_char(int val) {
    if (val >= 0 && val < 16)
        return simbolos[val];
    return '?';
}

/* ============================================================================
   TXACPLAY - Estrutura e funções do player
   ============================================================================ */

typedef struct {
    FILE *file;
    
    // Informações do áudio
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t total_samples;
    
    // Buffer de texto decodificado
    char *texto_decodificado;
    size_t texto_capacity;
    size_t texto_length;
    
    // Buffer de amostras
    int32_t *samples;
    size_t samples_capacity;
    int sample_count;
    
    // Posição atual
    unsigned int sample_pos;
    unsigned int buffer_pos;
    
    // Estado de pausa
    int is_paused;
    
} txacplay_desc;

// Função para garantir capacidade do buffer de texto
void ensure_text_capacity(txacplay_desc *tp, size_t required_space) {
    if (tp->texto_length + required_space >= tp->texto_capacity) {
        size_t new_capacity = tp->texto_capacity == 0 ? 
            INITIAL_CHARS_CAPACITY : tp->texto_capacity * GROWTH_FACTOR;
        
        while (tp->texto_length + required_space >= new_capacity) {
            new_capacity *= GROWTH_FACTOR;
        }

        char *new_ptr = (char *)realloc(tp->texto_decodificado, new_capacity);
        if (new_ptr == NULL) {
            perror("Erro ao realocar memória para texto");
            exit(EXIT_FAILURE);
        }
        tp->texto_decodificado = new_ptr;
        tp->texto_capacity = new_capacity;
    }
}

// Função para garantir capacidade do buffer de amostras
void ensure_samples_capacity(txacplay_desc *tp, size_t required_space) {
    if (tp->sample_count + required_space >= tp->samples_capacity) {
        size_t new_capacity = tp->samples_capacity == 0 ? 
            INITIAL_SAMPLES_CAPACITY : tp->samples_capacity * GROWTH_FACTOR;
        
        while (tp->sample_count + required_space >= new_capacity) {
            new_capacity *= GROWTH_FACTOR;
        }

        int32_t *new_ptr = (int32_t *)realloc(tp->samples, new_capacity * sizeof(int32_t));
        if (new_ptr == NULL) {
            perror("Erro ao realocar memória para samples");
            exit(EXIT_FAILURE);
        }
        tp->samples = new_ptr;
        tp->samples_capacity = new_capacity;
    }
}

// Decodifica arquivo binário 4-bit para texto
void binario_para_texto(txacplay_desc *tp) {
    rewind(tp->file);
    tp->texto_length = 0;

    int byte;
    while ((byte = fgetc(tp->file)) != EOF) {
        ensure_text_capacity(tp, 2);

        int high = (byte >> 4) & 0x0F;
        int low  = byte & 0x0F;

        tp->texto_decodificado[tp->texto_length++] = bit4_para_char(high);
        tp->texto_decodificado[tp->texto_length++] = bit4_para_char(low);
    }

    ensure_text_capacity(tp, 1);
    tp->texto_decodificado[tp->texto_length] = '\0';
}

// Descompacta string e aplica ganho
void descompactar_chunk(txacplay_desc *tp, int max_samples) {
    char buffer[128];
    int len = tp->texto_length;
    tp->sample_count = 0;
    tp->buffer_pos = 0;

    while (tp->buffer_pos < len && tp->sample_count < max_samples) {
        int b = 0;
        while (tp->buffer_pos < len && tp->texto_decodificado[tp->buffer_pos] != ',') {
            if (b < sizeof(buffer) - 1) {
                buffer[b++] = tp->texto_decodificado[tp->buffer_pos++];
            } else {
                tp->buffer_pos++;
            }
        }
        buffer[b] = '\0';
        tp->buffer_pos++;

        double num_double;
        int rep;

        if (sscanf(buffer, "%lf^%d", &num_double, &rep) == 2) {
            // Padrão: número^repetições
            ensure_samples_capacity(tp, rep);
            double boosted_sample = num_double * AMPLITUDE_FACTOR;
            
            int32_t sample_val;
            if (boosted_sample > INT32_MAX_VAL) sample_val = INT32_MAX_VAL;
            else if (boosted_sample < INT32_MIN_VAL) sample_val = INT32_MIN_VAL;
            else sample_val = (int32_t)boosted_sample;
            
            for (int r = 0; r < rep && tp->sample_count < max_samples; r++) {
                tp->samples[tp->sample_count++] = sample_val;
            }

        } else if (sscanf(buffer, "%lf~%d", &num_double, &rep) == 2) {
            // Padrão: número~seguido
            ensure_samples_capacity(tp, rep + 2);
            double boosted_sample = num_double * AMPLITUDE_FACTOR;
            
            int32_t sample_val;
            if (boosted_sample > INT32_MAX_VAL) sample_val = INT32_MAX_VAL;
            else if (boosted_sample < INT32_MIN_VAL) sample_val = INT32_MIN_VAL;
            else sample_val = (int32_t)boosted_sample;
            
            tp->samples[tp->sample_count++] = sample_val;

            for (int r = 0; r < rep && tp->sample_count < max_samples; r++) {
                b = 0;
                while (tp->buffer_pos < len && tp->texto_decodificado[tp->buffer_pos] != ',') {
                    if (b < sizeof(buffer) - 1) {
                        buffer[b++] = tp->texto_decodificado[tp->buffer_pos++];
                    } else {
                        tp->buffer_pos++;
                    }
                }
                buffer[b] = '\0';
                tp->buffer_pos++;

                double temp_double;
                if (sscanf(buffer, "%lf", &temp_double) == 1) {
                    double boosted_temp = temp_double * AMPLITUDE_FACTOR;
                    
                    int32_t temp_val;
                    if (boosted_temp > INT32_MAX_VAL) temp_val = INT32_MAX_VAL;
                    else if (boosted_temp < INT32_MIN_VAL) temp_val = INT32_MIN_VAL;
                    else temp_val = (int32_t)boosted_temp;
                    
                    tp->samples[tp->sample_count++] = temp_val;
                }
            }
            
            if (tp->sample_count < max_samples) {
                tp->samples[tp->sample_count++] = sample_val;
            }

        } else if (sscanf(buffer, "%lf", &num_double) == 1) {
            // Padrão: apenas um número
            ensure_samples_capacity(tp, 1);
            double boosted_sample = num_double * AMPLITUDE_FACTOR;
            
            int32_t sample_val;
            if (boosted_sample > INT32_MAX_VAL) sample_val = INT32_MAX_VAL;
            else if (boosted_sample < INT32_MIN_VAL) sample_val = INT32_MIN_VAL;
            else sample_val = (int32_t)boosted_sample;
            
            tp->samples[tp->sample_count++] = sample_val;
        }
    }
}

txacplay_desc *txacplay_open(const char *path, uint32_t sample_rate, uint16_t channels) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Erro ao abrir arquivo: %s\n", path);
        return NULL;
    }

    txacplay_desc *tp = malloc(sizeof(txacplay_desc));
    memset(tp, 0, sizeof(txacplay_desc));
    
    tp->file = file;
    tp->sample_rate = sample_rate;
    tp->channels = channels;
    tp->sample_pos = 0;
    tp->is_paused = 0; // Começa sem pausar
    
    // Aloca buffers iniciais
    ensure_text_capacity(tp, 0);
    ensure_samples_capacity(tp, 0);
    
    // Decodifica arquivo binário para texto
    printf("Decodificando arquivo binário...\n");
    binario_para_texto(tp);
    printf("Arquivo decodificado. Tamanho do texto: %zu bytes\n", tp->texto_length);
    
    // Descompacta tudo para calcular total de amostras
    printf("Descompactando e aplicando ganho de %.1f dB...\n", GAIN_DB);
    descompactar_chunk(tp, INT32_MAX_VAL);
    tp->total_samples = tp->sample_count;
    printf("Total de amostras: %u\n", tp->total_samples);
    
    // Reinicia para playback
    tp->sample_pos = 0;
    
    return tp;
}

void txacplay_close(txacplay_desc *tp) {
    if (tp) {
        if (tp->file) fclose(tp->file);
        if (tp->texto_decodificado) free(tp->texto_decodificado);
        if (tp->samples) free(tp->samples);
        free(tp);
    }
}

void txacplay_rewind(txacplay_desc *tp) {
    tp->sample_pos = 0;
}

unsigned int txacplay_decode(txacplay_desc *tp, float *sample_data, int num_samples) {
    // Se estiver pausado, retorna silêncio
    if (tp->is_paused) {
        memset(sample_data, 0, num_samples * sizeof(float));
        return num_samples;
    }
    
    int samples_written = 0;
    
    // Fator de normalização constante (1 / 2^31)
    const float normalization_factor = 1.0f / 2147483648.0f;
    
    // Vetor AVX com o fator de normalização replicado 8 vezes
    __m256 norm_vec = _mm256_set1_ps(normalization_factor);
    
    while (samples_written < num_samples) {
        // Se chegou no fim, faz loop
        if (tp->sample_pos >= tp->total_samples) {
            txacplay_rewind(tp);
        }
        
        // Calcula quantas amostras podemos copiar sem passar do final
        int samples_remaining = tp->total_samples - tp->sample_pos;
        int samples_to_copy = num_samples - samples_written;
        
        if (samples_to_copy > samples_remaining) {
            samples_to_copy = samples_remaining;
        }
        
        // --- PROCESSAMENTO AVX ---
        // Processa blocos de 8 samples por vez
        int avx_blocks = samples_to_copy / AVX_BLOCK_SIZE;
        int avx_processed = 0;
        
        for (int block = 0; block < avx_blocks; block++) {
            int src_idx = tp->sample_pos + avx_processed;
            int dst_idx = samples_written + avx_processed;
            
            // 1. Carrega 8 samples (int32) do buffer
            __m256i int_vec = _mm256_loadu_si256((__m256i*)&tp->samples[src_idx]);
            
            // 2. Converte int32 para float32
            __m256 float_vec = _mm256_cvtepi32_ps(int_vec);
            
            // 3. Normaliza (multiplica pelo fator)
            __m256 normalized_vec = _mm256_mul_ps(float_vec, norm_vec);
            
            // 4. Armazena no buffer de saída
            _mm256_storeu_ps(&sample_data[dst_idx], normalized_vec);
            
            avx_processed += AVX_BLOCK_SIZE;
        }
        
        // --- PROCESSAMENTO SEQUENCIAL (CAUDA) ---
        // Processa as amostras restantes (< 8)
        int remaining = samples_to_copy - avx_processed;
        for (int i = 0; i < remaining; i++) {
            int src_idx = tp->sample_pos + avx_processed + i;
            int dst_idx = samples_written + avx_processed + i;
            
            int32_t sample = tp->samples[src_idx];
            sample_data[dst_idx] = sample * normalization_factor;
        }
        
        tp->sample_pos += samples_to_copy;
        samples_written += samples_to_copy;
    }
    
    return num_samples;
}

double txacplay_get_duration(txacplay_desc *tp) {
    // Total de amostras dividido pelos canais para obter tempo real
    return (double)tp->total_samples / (double)(tp->sample_rate * tp->channels);
}

double txacplay_get_time(txacplay_desc *tp) {
    // Posição atual dividida pelos canais para obter tempo real
    return (double)tp->sample_pos / (double)(tp->sample_rate * tp->channels);
}

void txacplay_seek(txacplay_desc *tp, double time_seconds) {
    // Multiplica pelo número de canais porque samples está entrelaçado
    unsigned int target_sample = (unsigned int)(time_seconds * tp->sample_rate * tp->channels);
    if (target_sample > tp->total_samples) {
        target_sample = tp->total_samples;
    }
    tp->sample_pos = target_sample;
}

/* ============================================================================
   CÓDIGO DA APLICAÇÃO
   ============================================================================ */

// getch() para windows/mac/linux
#if defined(_WIN32)
    #include <conio.h>
#else
    #if defined(__APPLE__)
        #include <unistd.h>
    #endif
    #include <termios.h>
    int getch(void) {
        struct termios oldattr, newattr;
        int ch;
        tcgetattr(STDIN_FILENO, &oldattr);
        newattr = oldattr;
        newattr.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
        ch = getchar();
        tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);
        return ch;
    }
#endif

void txacplay_toggle_pause(txacplay_desc *tp) {
    tp->is_paused = !tp->is_paused;
    if (tp->is_paused) {
        printf("\n⏸️  PAUSADO\n");
    } else {
        printf("\n▶️  REPRODUZINDO\n");
    }
    fflush(stdout);
}

// Callback do Sokol Audio
static void sokol_audio_cb(float* sample_data, int num_samples, int num_channels, void *user_data) {
    txacplay_desc *txacplay = (txacplay_desc *)user_data;
    
    if (num_channels != txacplay->channels) {
        printf("Audio cb channels %d não igual txac channels %d\n", num_channels, txacplay->channels);
        exit(1);
    }

    // num_samples aqui é o número de FRAMES (não samples individuais)
    // Para stereo: num_samples frames = num_samples * 2 samples totais
    int total_samples_needed = num_samples * num_channels;
    
    txacplay_decode(txacplay, sample_data, total_samples_needed);

    printf("\r %.2f / %.2f sec", txacplay_get_time(txacplay), txacplay_get_duration(txacplay));
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Uso: txacplay <arquivo.txac> [sample_rate] [channels]\n");
        printf("Exemplo: txacplay audio.txac 44100 1\n");
        exit(1);
    }

    // Parâmetros padrão
    uint32_t sample_rate = 44100;
    uint16_t channels = 1;
    
    if (argc >= 3) {
        sample_rate = atoi(argv[2]);
    }
    if (argc >= 4) {
        channels = atoi(argv[3]);
    }

    printf("Abrindo arquivo TXAC: %s\n", argv[1]);
    printf("Sample rate: %u Hz, Canais: %u\n", sample_rate, channels);
    
    txacplay_desc *txacplay = txacplay_open(argv[1], sample_rate, channels);

    if (!txacplay) {
        printf("Falha ao carregar %s\n", argv[1]);
        exit(1);
    }

    printf("\n=== INFORMAÇÕES DO ÁUDIO ===\n");
    printf("Arquivo: %s\n", argv[1]);
    printf("Canais: %d\n", txacplay->channels);
    printf("Sample rate: %d Hz\n", txacplay->sample_rate);
    printf("Total de amostras (entrelaçadas): %u\n", txacplay->total_samples);
    printf("Amostras por canal: %u\n", txacplay->total_samples / txacplay->channels);
    printf("Duração: %.2f segundos\n", txacplay_get_duration(txacplay));
    printf("\n🎵 Controles:\n");
    printf("  [ESPAÇO] pausar/retomar\n");
    printf("  [x] voltar 5s\n");
    printf("  [c] avançar 5s\n");
    printf("  [q] sair\n");
    printf("\n⚡ Otimização AVX ativada!\n\n");

    saudio_setup(&(saudio_desc){
        .sample_rate = txacplay->sample_rate,
        .num_channels = txacplay->channels,
        .stream_userdata_cb = sokol_audio_cb,
        .user_data = txacplay,
        .buffer_frames = 2048  // Aumenta o buffer para evitar cintilação
    });

    int wants_to_quit = 0;
    while (!wants_to_quit) {
        char c = getch();
        switch (c) {
            case ' ':  // Barra de espaço
                txacplay_toggle_pause(txacplay);
                break;
            case 'c': {
                double current = txacplay_get_time(txacplay);
                txacplay_seek(txacplay, current + 5.0);
                break;
            }
            case 'x': {
                double current = txacplay_get_time(txacplay);
                txacplay_seek(txacplay, current - 5.0);
                break;
            }
            case 'q': 
                wants_to_quit = 1; 
                break;
        }
    }

    saudio_shutdown();
    txacplay_close(txacplay);

    printf("\n");
    return 0;
}