#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h> // Para usar pow()
#include <immintrin.h> // Para intrínsecos AVX (ex: _mm256_*)

// Tamanhos iniciais e para realocação (podem ser ajustados)
#define INITIAL_CHARS_CAPACITY 1024 * 1024  // 1 MB inicial para o texto
#define INITIAL_SAMPLES_CAPACITY 512 * 1024 // 512 KB inicial para as amostras
#define GROWTH_FACTOR 2                   // Fator de crescimento para realocação (dobra o tamanho)

// Valor de ganho em decibéis
#define GAIN_DB 110.0f
#define AMPLITUDE_FACTOR pow(10.0f, GAIN_DB / 20.0f)

// Limites para int32_t para evitar clipping
#define INT32_MAX_VAL 2147483647
#define INT32_MIN_VAL -2147483648

// Declarando os ponteiros e suas capacidades e contadores
char *texto_decodificado = NULL;
size_t texto_capacity = 0; // Capacidade atual do buffer de texto
size_t texto_length = 0;   // Comprimento atual do texto

int32_t *samples = NULL;
size_t samples_capacity = 0; // Capacidade atual do buffer de amostras
int sample_count = 0;        // Contador de amostras de áudio



// Novo mapa de 16 símbolos válidos
const char simbolos[16] = {
    '0','1','2','3','4','5','6','7','8','9',
    ',', '^', '~', '(', ')', '-'
};

// Converte valor de 4 bits para caractere
char bit4_para_char(int val) {
    if (val >= 0 && val < 16)
        return simbolos[val];
    return '?'; // Caractere inválido ou erro
}



// Função para garantir que o buffer de texto tenha capacidade suficiente
void ensure_text_capacity(size_t required_space) {
    if (texto_length + required_space >= texto_capacity) {
        size_t new_capacity = texto_capacity == 0 ? INITIAL_CHARS_CAPACITY : texto_capacity * GROWTH_FACTOR;
        while (texto_length + required_space >= new_capacity) {
            new_capacity *= GROWTH_FACTOR;
        }

        char *new_ptr = (char *)realloc(texto_decodificado, new_capacity);
        if (new_ptr == NULL) {
            perror("Erro ao realocar memória para texto_decodificado");
            free(texto_decodificado); // Libera o que foi alocado antes de sair
            exit(EXIT_FAILURE);
        }
        texto_decodificado = new_ptr;
        texto_capacity = new_capacity;
        printf("Debug: Capacidade de texto realocada para %zu bytes.\n", texto_capacity);
    }
}

// Função corrigida: Lê um arquivo binário (4 bits por símbolo) e escreve
// os caracteres decodificados no buffer de texto dinamicamente alocado.
void binario_para_texto(const char *entrada_bin) {
    FILE *fin = fopen(entrada_bin, "rb");
    if (!fin) {
        perror("Erro ao abrir arquivo binário de entrada");
        exit(EXIT_FAILURE);
    }

    int byte;
    texto_length = 0; // Resetar o comprimento do texto antes de preencher

    // Loop para ler cada byte do arquivo binário
    while ((byte = fgetc(fin)) != EOF) {
        ensure_text_capacity(2); // Precisamos de espaço para 2 caracteres + '\0' (depois)

        int high = (byte >> 4) & 0x0F; // Extrai os 4 bits mais significativos
        int low  = byte & 0x0F;        // Extrai os 4 bits menos significativos

        // Converte e armazena os caracteres no buffer
        texto_decodificado[texto_length++] = bit4_para_char(high);
        texto_decodificado[texto_length++] = bit4_para_char(low);
    }

    ensure_text_capacity(1); // Garante espaço para o terminador nulo
    texto_decodificado[texto_length] = '\0'; // Adiciona o terminador de string ao final
    fclose(fin); // Fecha o arquivo de entrada
}



// Função para garantir que o buffer de amostras tenha capacidade suficiente
void ensure_samples_capacity(size_t required_space) {
    if (sample_count + required_space >= samples_capacity) {
        size_t new_capacity = samples_capacity == 0 ? INITIAL_SAMPLES_CAPACITY : samples_capacity * GROWTH_FACTOR;
        while (sample_count + required_space >= new_capacity) {
            new_capacity *= GROWTH_FACTOR;
        }

        int32_t *new_ptr = (int32_t *)realloc(samples, new_capacity * sizeof(int32_t));
        if (new_ptr == NULL) {
            perror("Erro ao realocar memória para samples");
            free(samples); // Libera o que foi alocado antes de sair
            exit(EXIT_FAILURE);
        }
        samples = new_ptr;
        samples_capacity = new_capacity;
        printf("Debug: Capacidade de samples realocada para %zu amostras.\n", samples_capacity);
    }
}

// Descompacta a string de texto (global) para o array global 'samples'
// Agora aplica o ganho de 30 dB e previne o clipping, com otimização AVX.
void descompactar_string() {
    char buffer[128]; // Buffer temporário para partes da string
    int i = 0;
    int len = texto_length;
    sample_count = 0;

    // --- VARIÁVEIS AVX ---
    // Usamos float (32 bits) para 8 elementos (256 bits)
    const int AVX_BLOCK_SIZE = 8;
    
    // Calcula o fator AVX apenas uma vez (deve ser float)
    float factor_float = (float)AMPLITUDE_FACTOR;
    
    // Vetores constantes para AVX
    // 1. Fator de multiplicação (o ganho)
    __m256 factor_vec = _mm256_set1_ps(factor_float);
    
    // 2. Limites de clipping (convertidos para int32_t vector)
    __m256i max_vec = _mm256_set1_epi32(INT32_MAX_VAL);
    __m256i min_vec = _mm256_set1_epi32(INT32_MIN_VAL);
    // ----------------------

    while (i < len) {
        // ... (Lógica de parsing permanece a mesma)
        int b = 0;
        while (i < len && texto_decodificado[i] != ',') {
            if (b < sizeof(buffer) - 1) {
                buffer[b++] = texto_decodificado[i++];
            } else {
                fprintf(stderr, "Aviso: Buffer interno de descompactação cheio. Ignorando excesso.\n");
                i++;
            }
        }
        buffer[b] = '\0';
        i++;

        double num_double;
        int rep;

        // Tenta decodificar padrões de compactação
        if (sscanf(buffer, "%lf^%d", &num_double, &rep) == 2) {
            // Padrão: número^repetições (ex: 100^5)
            
            // Garante capacidade antes de adicionar qualquer coisa
            ensure_samples_capacity(rep);
            
            // Vetor de float replicado com o valor base (num_double)
            __m256 sample_float_vec = _mm256_set1_ps((float)num_double);

            // --- PROCESSAMENTO AVX ---
            int avx_reps = rep / AVX_BLOCK_SIZE;
            
            for (int r = 0; r < avx_reps; r++) {
                // 1. Aplicar Ganho (Multiplicação float vetorizada)
                __m256 boosted_float_vec = _mm256_mul_ps(sample_float_vec, factor_vec);
                
                // 2. Converter para Inteiro 32-bit (com truncamento)
                __m256i boosted_int_vec = _mm256_cvttps_epi32(boosted_float_vec);

                // 3. Aplicar Clipping (max(min_val) e depois min(max_val) vetorizado)
                // Garante que não é menor que INT32_MIN
                __m256i clipped_min = _mm256_max_epi32(min_vec, boosted_int_vec); 
                // Garante que não é maior que INT32_MAX
                __m256i clipped_result = _mm256_min_epi32(max_vec, clipped_min); 

                // 4. Armazenar o resultado (8 amostras) no buffer
                // Usamos storeu (unaligned store) para segurança, já que samples pode não ser 32-byte alinhado
                _mm256_storeu_si256((__m256i*)&samples[sample_count], clipped_result);
                sample_count += AVX_BLOCK_SIZE;
            }

            // --- PROCESSAMENTO SEQUENCIAL (CAUDA) ---
            // Processa o restante das amostras (< 8)
            int remaining_reps = rep % AVX_BLOCK_SIZE;
            double boosted_sample_seq = num_double * AMPLITUDE_FACTOR; // Pré-calcula o valor sequencial
            
            for (int r = 0; r < remaining_reps; r++) {
                // Aplica clipping sequencialmente
                if (boosted_sample_seq > INT32_MAX_VAL) samples[sample_count++] = INT32_MAX_VAL;
                else if (boosted_sample_seq < INT32_MIN_VAL) samples[sample_count++] = INT32_MIN_VAL;
                else samples[sample_count++] = (int32_t)boosted_sample_seq;
            }

        } else if (sscanf(buffer, "%lf~%d", &num_double, &rep) == 2) {
            // Padrão: número~seguido por 'rep' outros números (sem vetorização AVX)
            
            // ... (CÓDIGO ORIGINAL PARA ~ PERMANECE INALTERADO)
            ensure_samples_capacity(1);
            double boosted_sample_initial = num_double * AMPLITUDE_FACTOR;
            
            if (boosted_sample_initial > INT32_MAX_VAL) samples[sample_count++] = INT32_MAX_VAL;
            else if (boosted_sample_initial < INT32_MIN_VAL) samples[sample_count++] = INT32_MIN_VAL;
            else samples[sample_count++] = (int32_t)boosted_sample_initial;

            for (int r = 0; r < rep; r++) {
                b = 0;
                while (i < len && texto_decodificado[i] != ',') {
                    if (b < sizeof(buffer) - 1) {
                        buffer[b++] = texto_decodificado[i++];
                    } else {
                        fprintf(stderr, "Aviso: Buffer interno de descompactação (follow-up) cheio. Ignorando excesso.\n");
                        i++;
                    }
                }
                buffer[b] = '\0';
                i++;

                double temp_double;
                if (sscanf(buffer, "%lf", &temp_double) == 1) {
                    ensure_samples_capacity(1);
                    double boosted_temp = temp_double * AMPLITUDE_FACTOR;
                    
                    if (boosted_temp > INT32_MAX_VAL) samples[sample_count++] = INT32_MAX_VAL;
                    else if (boosted_temp < INT32_MIN_VAL) samples[sample_count++] = INT32_MIN_VAL;
                    else samples[sample_count++] = (int32_t)boosted_temp;
                } else {
                     fprintf(stderr, "Aviso: Erro de parsing no valor de série: '%s'. Ignorando.\n", buffer);
                }
            }
            ensure_samples_capacity(1);
            samples[sample_count++] = (int32_t)boosted_sample_initial;

        } else if (sscanf(buffer, "%lf", &num_double) == 1) {
            // Padrão: apenas um número (sem vetorização AVX)
            
            // ... (CÓDIGO ORIGINAL PARA VALOR ÚNICO PERMANECE INALTERADO)
            ensure_samples_capacity(1);
            double boosted_sample = num_double * AMPLITUDE_FACTOR;
            
            if (boosted_sample > INT32_MAX_VAL) samples[sample_count++] = INT32_MAX_VAL;
            else if (boosted_sample < INT32_MIN_VAL) samples[sample_count++] = INT32_MIN_VAL;
            else samples[sample_count++] = (int32_t)boosted_sample;
        } else {
            fprintf(stderr, "Erro de parsing: '%s' não corresponde a nenhum padrão de descompactação. Ignorando.\n", buffer);
        }
    }
}



// Salva o array global 'samples' em um arquivo WAV
void salvar_wav(const char *saida_wav) {
    FILE *fwav = fopen(saida_wav, "wb");
    if (!fwav) {
        perror("Erro ao criar arquivo WAV");
        return;
    }
    int dih;
    int bruh;
    // Prompt for sample rate (default: 44100)
    printf("Coloque quantos hz o audio tem: ");
    scanf("%d", &dih);
    printf("Coloque quantos canais o audio tem: ");
    scanf("%d", &bruh);
    // Parâmetros do formato WAV
    uint32_t sampleRate = dih;
    uint16_t numChannels = bruh;   // Mono
    uint16_t bitsPerSample = 32;   // int32_t
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8); // Bytes por segundo
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);           // Bytes por bloco (amostra)

    // Cabeçalho WAV básico (RIFF, WAVE, fmt, data chunks)
    uint8_t cabecalho[44];
    memset(cabecalho, 0, sizeof(cabecalho));

    // Chunk ID "RIFF"
    memcpy(cabecalho, "RIFF", 4);
    // Tamanho total do arquivo (será preenchido depois)
    // cabecalho[4-7]
    // Format "WAVE"
    memcpy(cabecalho + 8, "WAVE", 4);

    // Subchunk1 ID "fmt "
    memcpy(cabecalho + 12, "fmt ", 4);
    // Subchunk1 Size (16 para PCM)
    uint32_t subchunk1Size = 16;
    memcpy(cabecalho + 16, &subchunk1Size, 4);
    // Audio Format (1 para PCM)
    uint16_t audioFormat = 1;
    memcpy(cabecalho + 20, &audioFormat, 2);
    // Number of Channels
    memcpy(cabecalho + 22, &numChannels, 2);
    // Sample Rate
    memcpy(cabecalho + 24, &sampleRate, 4);
    // Byte Rate
    memcpy(cabecalho + 28, &byteRate, 4);
    // Block Align
    memcpy(cabecalho + 32, &blockAlign, 2);
    // Bits Per Sample
    memcpy(cabecalho + 34, &bitsPerSample, 2);

    // Subchunk2 ID "data"
    memcpy(cabecalho + 36, "data", 4);
    // Subchunk2 Size (tamanho dos dados, será preenchido depois)
    // cabecalho[40-43]

    // Escreve o cabeçalho no arquivo
    fwrite(cabecalho, 1, 44, fwav);

    // Escreve os dados das amostras
    for (int i = 0; i < sample_count; i++) {
        fwrite(&samples[i], sizeof(int32_t), 1, fwav);
    }

    // Calcula os tamanhos finais
    int32_t tamanho_dados = sample_count * sizeof(int32_t);
    int32_t tamanho_total = 36 + tamanho_dados; // 36 bytes antes do chunk de dados

    // Volta e preenche os tamanhos corretos no cabeçalho
    fseek(fwav, 4, SEEK_SET);     // Posição para FileSize
    fwrite(&tamanho_total, 4, 1, fwav);

    fseek(fwav, 40, SEEK_SET);    // Posição para DataSize
    fwrite(&tamanho_dados, 4, 1, fwav);

    fclose(fwav);
    printf("✅ Arquivo WAV salvo com %d amostras em '%s'\n", sample_count, saida_wav);
}



int main() {
    const char *entrada_bin_file = "compactado_4bits.txac";
    const char *saida_wav_file = "restaurado_4bits.wav";



    // Aloca a memória inicial para os buffers
    ensure_text_capacity(0); // Aloca a capacidade inicial para texto_decodificado
    ensure_samples_capacity(0); // Aloca a capacidade inicial para samples

    printf("Iniciando a decodificação do arquivo binário...\n");
    binario_para_texto(entrada_bin_file);
    printf("Decodificação binária concluída. Tamanho do texto: %zu\n", texto_length);

    printf("Iniciando a descompactação da string e aplicação de ganho de %.1f dB...\n", GAIN_DB);
    descompactar_string();
    printf("Descompactação e ganho concluídos. Total de amostras: %d\n", sample_count);

    printf("Salvando arquivo WAV...\n");
    salvar_wav(saida_wav_file);

    // Lembre-se de liberar a memória alocada dinamicamente
    free(texto_decodificado);
    free(samples);

    return 0;
}