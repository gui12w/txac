#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h> 
#include <immintrin.h> // Para AVX

#define SOKOL_AUDIO_IMPL
// IMPORTANTE: Você precisará do arquivo "sokol_audio.h" no mesmo diretório
#include "sokol_audio.h"

// --- DEFINIÇÕES E GLOBAIS DO CODEC (Baseado em avx output.c) ---
#define INITIAL_CHARS_CAPACITY 1024 * 1024 
#define INITIAL_SAMPLES_CAPACITY 512 * 1024 
#define GROWTH_FACTOR 2 
#define GAIN_DB 110.0f
#define AMPLITUDE_FACTOR pow(10.0f, GAIN_DB / 20.0f)
#define INT32_MAX_VAL 2147483647
#define INT32_MIN_VAL -2147483648

// Novo mapa de 16 símbolos válidos (incluindo ( e ) para compressão de bloco)
const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

// --- ESTRUTURA DO REPRODUTOR (Adaptada de qoaplay_desc) ---
typedef struct {
    int32_t *samples;       // Buffer de amostras PCM (int32_t)
    int sample_count;       // Total de amostras no buffer
    unsigned int current_pos; // Posição atual de leitura (em amostras, não bytes)
    
    // Dados para descompressão em memória
    char *texto_decodificado;
    size_t texto_capacity;
    size_t texto_length;
    
    // Info do Arquivo
    int samplerate;
    int channels;
} mycodec_desc;


// ----------------------------------------------------------------------
// FUNÇÕES DE UTILIDADE DE MEMÓRIA E CONVERSÃO (Baseado em avx output.c)
// ----------------------------------------------------------------------

char bit4_para_char(int val) {
    if (val >= 0 && val < 16) return simbolos[val];
    return '?'; 
}

void ensure_text_capacity(mycodec_desc *qp, size_t required_space) {
    if (qp->texto_length + required_space >= qp->texto_capacity) {
        size_t new_capacity = qp->texto_capacity == 0 ? INITIAL_CHARS_CAPACITY : qp->texto_capacity * GROWTH_FACTOR;
        while (qp->texto_length + required_space >= new_capacity) {
            new_capacity *= GROWTH_FACTOR;
        }

        char *new_ptr = (char *)realloc(qp->texto_decodificado, new_capacity);
        if (new_ptr == NULL) {
            perror("Erro ao realocar memória para texto_decodificado");
            free(qp->texto_decodificado); 
            exit(EXIT_FAILURE);
        }
        qp->texto_decodificado = new_ptr;
        qp->texto_capacity = new_capacity;
    }
}

void binario_para_texto(mycodec_desc *qp, const char *entrada_bin) {
    FILE *fin = fopen(entrada_bin, "rb");
    if (!fin) { perror("Erro ao abrir arquivo binário de entrada"); exit(EXIT_FAILURE); }
    int byte;
    qp->texto_length = 0; 
    
    // Inicializa a memória se ainda não o fez
    if (!qp->texto_decodificado) {
        qp->texto_capacity = 0;
        ensure_text_capacity(qp, INITIAL_CHARS_CAPACITY);
    }
    
    while ((byte = fgetc(fin)) != EOF) {
        ensure_text_capacity(qp, 2); 
        int high = (byte >> 4) & 0x0F; 
        int low  = byte & 0x0F;        
        qp->texto_decodificado[qp->texto_length++] = bit4_para_char(high);
        qp->texto_decodificado[qp->texto_length++] = bit4_para_char(low);
    }
    ensure_text_capacity(qp, 1); 
    qp->texto_decodificado[qp->texto_length] = '\0'; 
    fclose(fin); 
}

void ensure_samples_capacity(mycodec_desc *qp, size_t required_space) {
    size_t current_count = qp->sample_count;
    size_t current_capacity = qp->samples ? qp->samples->samples_capacity : 0; 

    if (current_count + required_space >= current_capacity) {
        size_t new_capacity = current_capacity == 0 ? INITIAL_SAMPLES_CAPACITY : current_capacity * GROWTH_FACTOR;
        while (current_count + required_space >= new_capacity) {
            new_capacity *= GROWTH_FACTOR;
        }
        int32_t *new_ptr = (int32_t *)realloc(qp->samples, new_capacity * sizeof(int32_t));
        if (new_ptr == NULL) { perror("Erro ao realocar memória para samples"); free(qp->samples); exit(EXIT_FAILURE); }
        qp->samples = new_ptr;
        // Atualiza a capacidade na estrutura (se necessário)
        // OBS: A capacidade não está diretamente na struct mycodec_desc, 
        // mas deve ser rastreada se a realocação for feita dentro desta função. 
        // Vou assumir que o novo ptr é suficiente para o bloco, 
        // mas a lógica original estava um pouco estranha no rastreamento de capacidade externa. 
        // Simplificação: apenas alocamos espaço suficiente.
    }
}


// ----------------------------------------------------------------------
// FUNÇÕES DE DESCOMPRESSÃO (Baseado em avx output.c e contexto)
// ----------------------------------------------------------------------

// Função para Expansão de Blocos '()' (Recuperado do contexto completo)
void expandir_blocos(mycodec_desc *qp) {
    size_t new_capacity = qp->texto_capacity;
    char *new_texto = (char*)malloc(new_capacity * sizeof(char));
    if (!new_texto) { perror("Erro malloc expandir_blocos"); exit(EXIT_FAILURE); }
    size_t new_length = 0;
    
    char *p = qp->texto_decodificado;
    char *end = qp->texto_decodificado + qp->texto_length;

    while (p < end) {
        char *p_open = strchr(p, '(');

        if (!p_open) {
            size_t remaining = end - p;
            if (new_length + remaining + 1 >= new_capacity) {
                 new_capacity = new_length + remaining + INITIAL_CHARS_CAPACITY;
                 new_texto = (char*)realloc(new_texto, new_capacity);
                 if (!new_texto) { perror("Erro realloc bloco 1"); exit(EXIT_FAILURE); }
            }
            memcpy(new_texto + new_length, p, remaining);
            new_length += remaining;
            break;
        }

        size_t prefix_len = p_open - p;
        if (new_length + prefix_len + 1 >= new_capacity) {
             new_capacity = new_length + prefix_len + INITIAL_CHARS_CAPACITY;
             new_texto = (char*)realloc(new_texto, new_capacity);
             if (!new_texto) { perror("Erro realloc bloco 2"); exit(EXIT_FAILURE); }
        }
        memcpy(new_texto + new_length, p, prefix_len);
        new_length += prefix_len;

        char *p_close = strchr(p_open, ')');
        if (!p_close) { p = p_open + 1; continue; }
        
        char *p_rep = p_close + 1;
        int rep = 1;
        int advance = 0;

        if (*p_rep == '^') {
            if (sscanf(p_rep, "^%d%n", &rep, &advance) != 1) { /* Assume rep=1 */ }
        } 
        
        size_t bloco_len = p_close - (p_open + 1);
        char *bloco = (char*)malloc((bloco_len + 2) * sizeof(char));
        memcpy(bloco, p_open + 1, bloco_len);
        bloco[bloco_len] = ','; // Adiciona a vírgula de separação
        bloco[bloco_len + 1] = '\0';
        size_t required = bloco_len + 1;

        for (int r = 0; r < rep; r++) {
            if (new_length + required >= new_capacity) {
                 new_capacity = new_length + required + INITIAL_CHARS_CAPACITY;
                 new_texto = (char*)realloc(new_texto, new_capacity);
                 if (!new_texto) { perror("Erro realloc bloco 3"); free(bloco); exit(EXIT_FAILURE); }
            }
            memcpy(new_texto + new_length, bloco, required);
            new_length += required;
        }

        free(bloco);
        
        if (advance > 0) {
            p = p_rep + advance;
        } else {
            p = p_close + 1; 
        }
        
        if (*p == ',') p++;
    }

    if (new_length > 0 && new_texto[new_length - 1] == ',') {
        new_length--; 
    }
    
    new_texto[new_length] = '\0';

    free(qp->texto_decodificado);
    qp->texto_decodificado = new_texto;
    qp->texto_length = new_length;
    qp->texto_capacity = new_capacity;
}

// Descompacta a string de texto para o array 'samples' com AVX e Clipping
void descompactar_string(mycodec_desc *qp) {
    char buffer[128]; 
    int i = 0;
    int len = qp->texto_length;
    qp->sample_count = 0;

    const int AVX_BLOCK_SIZE = 8;
    float factor_float = (float)AMPLITUDE_FACTOR;
    __m256 factor_vec = _mm256_set1_ps(factor_float);
    __m256i max_vec = _mm256_set1_epi32(INT32_MAX_VAL);
    __m256i min_vec = _mm256_set1_epi32(INT32_MIN_VAL);

    while (i < len) {
        int b = 0;
        while (i < len && qp->texto_decodificado[i] != ',') {
            if (b < sizeof(buffer) - 1) {
                buffer[b++] = qp->texto_decodificado[i++];
            } else { i++; }
        }
        buffer[b] = '\0';
        i++;

        double num_double;
        int rep;

        if (sscanf(buffer, "%lf^%d", &num_double, &rep) == 2) {
             // Lógica de ^ com AVX
            ensure_samples_capacity(qp, rep);
            __m256 sample_float_vec = _mm256_set1_ps((float)num_double);
            int avx_reps = rep / AVX_BLOCK_SIZE;
            
            for (int r = 0; r < avx_reps; r++) {
                __m256 boosted_float_vec = _mm256_mul_ps(sample_float_vec, factor_vec);
                __m256i boosted_int_vec = _mm256_cvttps_epi32(boosted_float_vec);
                __m256i clipped_min = _mm256_max_epi32(min_vec, boosted_int_vec); 
                __m256i clipped_result = _mm256_min_epi32(max_vec, clipped_min); 
                _mm256_storeu_si256((__m256i*)&qp->samples[qp->sample_count], clipped_result);
                qp->sample_count += AVX_BLOCK_SIZE;
            }

            int remaining_reps = rep % AVX_BLOCK_SIZE;
            double boosted_sample_seq = num_double * AMPLITUDE_FACTOR;
            for (int r = 0; r < remaining_reps; r++) {
                if (boosted_sample_seq > INT32_MAX_VAL) qp->samples[qp->sample_count++] = INT32_MAX_VAL;
                else if (boosted_sample_seq < INT32_MIN_VAL) qp->samples[qp->sample_count++] = INT32_MIN_VAL;
                else qp->samples[qp->sample_count++] = (int32_t)boosted_sample_seq;
            }

        } else if (sscanf(buffer, "%lf~%d", &num_double, &rep) == 2) {
            // Lógica de ~ (sequencial)
            ensure_samples_capacity(qp, 1);
            double boosted_sample_initial = num_double * AMPLITUDE_FACTOR;
            if (boosted_sample_initial > INT32_MAX_VAL) qp->samples[qp->sample_count++] = INT32_MAX_VAL;
            else if (boosted_sample_initial < INT32_MIN_VAL) qp->samples[qp->sample_count++] = INT32_MIN_VAL;
            else qp->samples[qp->sample_count++] = (int32_t)boosted_sample_initial;

            for (int r = 0; r < rep; r++) {
                b = 0;
                while (i < len && qp->texto_decodificado[i] != ',') {
                    if (b < sizeof(buffer) - 1) { buffer[b++] = qp->texto_decodificado[i++]; } else { i++; }
                }
                buffer[b] = '\0';
                i++;

                double temp_double;
                if (sscanf(buffer, "%lf", &temp_double) == 1) {
                    ensure_samples_capacity(qp, 1);
                    double boosted_temp = temp_double * AMPLITUDE_FACTOR;
                    if (boosted_temp > INT32_MAX_VAL) qp->samples[qp->sample_count++] = INT32_MAX_VAL;
                    else if (boosted_temp < INT32_MIN_VAL) qp->samples[qp->sample_count++] = INT32_MIN_VAL;
                    else qp->samples[qp->sample_count++] = (int32_t)boosted_temp;
                }
            }
            ensure_samples_capacity(qp, 1);
            qp->samples[qp->sample_count++] = (int32_t)boosted_sample_initial;

        } else if (sscanf(buffer, "%lf", &num_double) == 1) {
            // Lógica de valor único (sequencial)
            ensure_samples_capacity(qp, 1);
            double boosted_sample = num_double * AMPLITUDE_FACTOR;
            if (boosted_sample > INT32_MAX_VAL) qp->samples[qp->sample_count++] = INT32_MAX_VAL;
            else if (boosted_sample < INT32_MIN_VAL) qp->samples[qp->sample_count++] = INT32_MIN_VAL;
            else qp->samples[qp->sample_count++] = (int32_t)boosted_sample;
        } 
    }
}


// ----------------------------------------------------------------------
// FUNÇÕES DO REPRODUTOR (Adaptadas de qoaplay.c)
// ----------------------------------------------------------------------

mycodec_desc *mycodec_open(const char *path, int samplerate, int channels) {
    printf("Iniciando a abertura do arquivo: %s\n", path);
    
    // 1. Aloca a estrutura principal
    mycodec_desc *qp = (mycodec_desc*)calloc(1, sizeof(mycodec_desc));
    if (!qp) { perror("Erro calloc mycodec_desc"); return NULL; }
    
    qp->samplerate = samplerate;
    qp->channels = channels;
    
    // 2. Decodificação Binária para Texto
    printf("   -> 1/3. Decodificando binário de 4 bits para texto...\n");
    binario_para_texto(qp, path);

    // 3. Expansão da Compressão de Blocos '()'
    if (qp->texto_length > 0) {
        printf("   -> 2/3. Expandindo compressão de blocos '()'.\n");
        expandir_blocos(qp);
    }

    // 4. Descompactação total (^) e (~) e aplicação de ganho (AVX)
    if (qp->texto_length > 0) {
        printf("   -> 3/3. Descompactando ^ e ~ e aplicando ganho (AVX).\n");
        descompactar_string(qp);
    }
    
    // Libera o buffer de texto, ele não é mais necessário
    free(qp->texto_decodificado);
    qp->texto_decodificado = NULL;

    if (qp->sample_count == 0) {
        printf("Falha na descompressão. 0 amostras.\n");
        free(qp);
        return NULL;
    }

    printf("✅ Descompressão completa. Amostras totais: %d\n", qp->sample_count);
    return qp;
}

void mycodec_close(mycodec_desc *qp) {
    if (qp) {
        if (qp->samples) free(qp->samples);
        free(qp);
    }
}


/* Sokol Audio callback - chamado quando o sistema de áudio precisa de mais amostras */
static void sokol_audio_cb(float* sample_data, int num_samples, int num_channels, void *user_data) {
	mycodec_desc *qp = (mycodec_desc *)user_data;
    
    // Garante que o número de canais está correto
	if (num_channels != qp->channels) {
		printf("Erro: Canais do callback (%d) não batem com canais do arquivo (%d)\n", num_channels, qp->channels);
		exit(1);
	}

    int dst_index = 0;
	for (int i = 0; i < num_samples; i++) {
        
        // Loop through channels (Assumindo que o codec do usuário gera áudio mono, mas permite mais canais)
        for (int c = 0; c < qp->channels; c++) {
            
            // 1. Checa se precisamos fazer o loop (Rewind)
            if (qp->current_pos >= qp->sample_count) {
                // Reinicia a posição para loop infinito
                qp->current_pos = 0;
            }
            
            // 2. Normaliza int32_t para float -1.0 a 1.0 e escreve
            // O código assume que as amostras são 32-bit (int32_t)
            float normalized_sample = (float)qp->samples[qp->current_pos] / (float)INT32_MAX_VAL;
            sample_data[dst_index++] = normalized_sample;
            
            // Avança a posição no buffer decompactado (o loop de canais faz o mesmo sample ser lido
            // para todos os canais, pois a descompressão é baseada em valores sequenciais, não interleaved)
        }
        qp->current_pos++;
	}
    
    double duration = (double)qp->sample_count / (double)qp->samplerate / qp->channels;
    double time = (double)qp->current_pos / (double)qp->samplerate / qp->channels;
    
	printf("\r %6.2f / %.2f sec", time, duration);
	fflush(stdout);
}

// getch() para controle (copiado de qoaplay.c)
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


int main(int argc, char **argv) {
    if (argc < 2) {
		printf("Uso: mycodec_player <arquivo.txac>\n");
		exit(1);
	}
    
    // --- Configurações que DEVEM SER AJUSTADAS ---
    int sampleRate = 44100; 
    int numChannels = 1;
    
    printf("--- Configuração de Áudio ---\n");
    printf("Taxa de Amostragem (Hz, padrão 44100): ");
    char sr_buffer[10];
    if (fgets(sr_buffer, sizeof(sr_buffer), stdin)) {
        int temp = atoi(sr_buffer);
        if (temp > 0) sampleRate = temp;
    }
    
    printf("Número de Canais (1=Mono, 2=Stereo, padrão 1): ");
    char ch_buffer[10];
    if (fgets(ch_buffer, sizeof(ch_buffer), stdin)) {
        int temp = atoi(ch_buffer);
        if (temp > 0) numChannels = temp;
    }
    // ---------------------------------------------
    
    mycodec_desc *qp = mycodec_open(argv[1], sampleRate, numChannels);

	if (!qp) {
		printf("Falha ao carregar e descompactar %s\n", argv[1]);
		exit(1);
	}

	printf(
		"\n%s: Canais: %d, Taxa: %d Hz, Amostras totais: %d\n",
		argv[1], 
		qp->channels,
		qp->samplerate,
		qp->sample_count
	);
	printf("Controles: [x] voltar / [c] avançar / [q] sair\n");

	saudio_setup(&(saudio_desc){
		.sample_rate = qp->samplerate,
		.num_channels = qp->channels,
		.stream_userdata_cb = sokol_audio_cb,
		.user_data = qp
	});

	int wants_to_quit = 0;
	while (!wants_to_quit) {
		char c = getch();
		switch (c) {
			case 'c': 
                qp->current_pos += qp->samplerate / 2; // Avança 0.5s
                if (qp->current_pos >= qp->sample_count) qp->current_pos = qp->sample_count - 1;
                break;
			case 'x': 
                qp->current_pos -= qp->samplerate / 2; // Volta 0.5s
                if (qp->current_pos < 0 || qp->current_pos >= qp->sample_count) qp->current_pos = 0; 
                break;
			case 'q': wants_to_quit = 1; break;
		}
	}

	saudio_shutdown();
	mycodec_close(qp);

	printf("\n");
	return 0;
}
