/*
TXAC Player v12.0 - Direct 4bit→Float Streaming
- Elimina etapa intermediária de texto completo
- Parsing direto: 4-bit → float buffer
- Menor uso de RAM e processamento mais rápido
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <immintrin.h>

#if defined(_WIN32)
    #include <windows.h>
    #define THREAD_SLEEP_MS(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define THREAD_SLEEP_MS(ms) usleep((ms) * 1000)
#endif

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

#define SOKOL_AUDIO_IMPL
#include "sokol_audio.h"

#define TXAC_MAGIC "TXAC"
#define MAX_CHANNELS 8
#define NORMALIZATION_DIVISOR 0.0002147483648f 

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
// BUFFER FLOAT DIRETO
// ============================================================================
typedef struct {
    float *data;        
    uint64_t capacity;
    uint64_t count;
} BufferFloat;

void init_buffer_float(BufferFloat *buf, uint64_t initial_capacity) {
    buf->capacity = initial_capacity;
    buf->count = 0;
    buf->data = (float*)malloc(buf->capacity * sizeof(float));
    if (!buf->data) {
        fprintf(stderr, "Error: Failed to allocate RAM for float buffer\n");
        exit(1);
    }
}

void ensure_buffer_capacity_float(BufferFloat *buf, uint64_t required) {
    if (buf->count + required >= buf->capacity) {
        uint64_t new_cap = buf->capacity * 2;
        while (buf->count + required >= new_cap) new_cap *= 2;
        
        float *new_ptr = (float*)realloc(buf->data, new_cap * sizeof(float));
        if (!new_ptr) {
            fprintf(stderr, "Error: Insuficient memory (RAM is full)\n");
            exit(1);
        }
        buf->data = new_ptr;
        buf->capacity = new_cap;
    }
}

void push_value_float(BufferFloat *buf, float value) {
    ensure_buffer_capacity_float(buf, 1);
    buf->data[buf->count++] = value;
}

// ============================================================================
// PARSER DIRETO DE 4-BIT → FLOAT
// ============================================================================
typedef struct {
    uint8_t *raw_data;  // Dados 4-bit brutos do arquivo
    size_t byte_count;
    size_t byte_pos;
    int nibble_pos;     // 0 = high nibble, 1 = low nibble
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
        // High nibble
        c = simbolos[(byte >> 4) & 0x0F];
        s->nibble_pos = 1;
    } else {
        // Low nibble
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
// DESCOMPRESSÃO DIRETA PARA FLOAT
// ============================================================================
typedef struct {
    int channel_id;
    uint8_t *compressed_4bit;
    size_t compressed_size;
    BufferFloat *output_buffer;
    pthread_t thread;
    volatile int finished;
} ChannelLoader;

void process_stream_to_float(ChannelLoader *ldr, Stream4Bit *stream, uint64_t *sample_idx, int recursion_depth) {
    if (recursion_depth > 100) return;
    
    char token[64];
    
    while (stream->byte_pos < stream->byte_count || stream->nibble_pos > 0) {
        read_token_stream(stream, token, 64);
        
        if (token[0] == '\0') continue;
        if (token[0] == '(' || token[0] == ')') continue;
        
        double num_d;
        int rep;
        float val_float;
        
        // Caso 1: Repetição (valor^repeticoes)
        if (sscanf(token, "%lf^%d", &num_d, &rep) == 2) {
            // Converte int32 → float AQUI (uma vez só)
            val_float = (float)((int32_t)num_d) * NORMALIZATION_DIVISOR;
            
            ensure_buffer_capacity_float(ldr->output_buffer, rep);
            for (int i = 0; i < rep; i++) {
                ldr->output_buffer->data[ldr->output_buffer->count++] = val_float;
                (*sample_idx)++;
            }
            if (recursion_depth > 0) return;
        }
        // Caso 2: Sniper/Loop (valor~distancia)
        else if (sscanf(token, "%lf~%d", &num_d, &rep) == 2) {
            val_float = (float)((int32_t)num_d) * NORMALIZATION_DIVISOR;
            push_value_float(ldr->output_buffer, val_float);
            (*sample_idx)++;
            
            for (int i = 0; i < rep; i++) {
                process_stream_to_float(ldr, stream, sample_idx, recursion_depth + 1);
            }
            
            push_value_float(ldr->output_buffer, val_float);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
        // Caso 3: Valor simples
        else if (sscanf(token, "%lf", &num_d) == 1) {
            val_float = (float)((int32_t)num_d) * NORMALIZATION_DIVISOR;
            push_value_float(ldr->output_buffer, val_float);
            (*sample_idx)++;
            if (recursion_depth > 0) return;
        }
    }
}

void *loader_thread_func(void *arg) {
    ChannelLoader *ldr = (ChannelLoader*)arg;
    uint64_t sample_idx = 0;
    
    printf("  [Channel %d] Decompressing 4bit to float directly...\n", ldr->channel_id);
    
    Stream4Bit stream;
    init_stream(&stream, ldr->compressed_4bit, ldr->compressed_size);
    process_stream_to_float(ldr, &stream, &sample_idx, 0);
    
    printf("  [Channel %d] %llu samples loaded\n", ldr->channel_id, (unsigned long long)sample_idx);
    ldr->finished = 1;
    return NULL;
}

// ============================================================================
// ESTRUTURAS PRINCIPAIS
// ============================================================================
typedef struct {
    FILE *file;
    TXACHeader header;
    float *pcm_data; // Buffer final intercalado JÁ EM FLOAT
    uint64_t total_samples;
    volatile uint64_t playback_cursor;
    volatile int is_paused;
    volatile int running;
    ChannelLoader loaders[MAX_CHANNELS];
    BufferFloat channel_buffers[MAX_CHANNELS];
} txacplay_desc;

// ============================================================================
// INTERCALAÇÃO (FLOAT)
// ============================================================================
void intercalar_canais_float(txacplay_desc *tp) {
    printf("\nInterleaving channels in RAM...\n");
    
    uint64_t frames = tp->channel_buffers[0].count;
    for (int i = 1; i < tp->header.channels; i++) {
        if (tp->channel_buffers[i].count < frames) {
            frames = tp->channel_buffers[i].count;
        }
    }

    tp->total_samples = frames * tp->header.channels;
    
    tp->pcm_data = (float *)malloc(tp->total_samples * sizeof(float));
    if (!tp->pcm_data) {
        printf("Fatal error: No RAM for final buffer. (%llu samples)\n", tp->total_samples);
        exit(1);
    }
    
    for (uint64_t f = 0; f < frames; f++) {
        for (int c = 0; c < tp->header.channels; c++) {
            tp->pcm_data[f * tp->header.channels + c] = tp->channel_buffers[c].data[f];
        }
    }
    
    double size_mb = (double)(tp->total_samples * sizeof(float)) / (1024.0 * 1024.0);
    printf("Ready: %.2f MB of RAM for audio decompressed.\n", size_mb);
}

// ============================================================================
// CÁLCULO DE TEMPO
// ============================================================================
double calculate_time(txacplay_desc *tp) {
    return (double)tp->playback_cursor / (double)(tp->header.sample_rate * tp->header.channels);
}

double calculate_duration(txacplay_desc *tp) {
    return (double)tp->total_samples / (double)(tp->header.sample_rate * tp->header.channels);
}

// ============================================================================
// CALLBACK SOKOL (SUPER OTIMIZADO - JÁ É FLOAT!)
// ============================================================================
void audio_cb(float *buffer, int num_frames, int num_channels, void *user_data) {
    txacplay_desc *tp = (txacplay_desc*)user_data;
    
    if (tp->is_paused || !tp->running || !tp->pcm_data) {
        memset(buffer, 0, num_frames * num_channels * sizeof(float));
        return;
    }
    
    int total_samples = num_frames * num_channels;
    
    // COPIA DIRETA - SEM CONVERSÃO!
    for (int i = 0; i < total_samples; i++) {
        if (tp->playback_cursor >= tp->total_samples) tp->playback_cursor = 0;
        buffer[i] = tp->pcm_data[tp->playback_cursor++];
    }
    double current = calculate_time(tp);
    double duration = calculate_duration(tp);
    printf("\r%.2f / %.2f sec", current, duration);
    fflush(stdout);
}

void toggle_pause(txacplay_desc *tp) {
    tp->is_paused = !tp->is_paused;
    if (tp->is_paused) {
        printf("\nPAUSED\n");
    } else {
        printf("\nPLAYING\n");
    }
}

void txacplay_seek_absolute(txacplay_desc *tp, double time_seconds) {
    double samples_per_second = (double)tp->header.sample_rate * tp->header.channels;
    uint64_t target_samples = (uint64_t)(time_seconds * samples_per_second);
    
    uint64_t remainder = target_samples % tp->header.channels;
    target_samples -= remainder;
    
    if (target_samples > tp->total_samples) {
        target_samples = tp->total_samples - (tp->total_samples % tp->header.channels);
    }
    if (time_seconds < 0) target_samples = 0;
    
    tp->playback_cursor = target_samples;
}

void update_timer(txacplay_desc *tp) {
    double current = calculate_time(tp);
    double duration = calculate_duration(tp);
    printf("\r%.2f / %.2f sec", current, duration);
    fflush(stdout);
}

// ============================================================================
// ABERTURA E SETUP
// ============================================================================
txacplay_desc *txacplay_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    txacplay_desc *tp = (txacplay_desc*)calloc(1, sizeof(txacplay_desc));
    tp->file = f;
    
    char magic[4]; fread(magic, 1, 4, f);
    uint32_t trash; fread(&trash, 4, 1, f);
    fread(&tp->header.sample_rate, 4, 1, f);
    fread(&tp->header.channels, 2, 1, f);
    fread(&tp->header.bits_per_sample, 2, 1, f);
    fread(&tp->header.flags, 4, 1, f);
    fread(&tp->header.total_samples, 8, 1, f);
    fseek(f, 36, SEEK_CUR);
    
    uint64_t offsets[MAX_CHANNELS], sizes[MAX_CHANNELS];
    for (int i = 0; i < tp->header.channels; i++) {
        fread(&offsets[i], 8, 1, f);
        fread(&sizes[i], 8, 1, f);
    }
    
    // Inicia threads que fazem parsing direto 4bit→float
    for (int i = 0; i < tp->header.channels; i++) {
        init_buffer_float(&tp->channel_buffers[i], tp->header.total_samples / tp->header.channels);
        
        tp->loaders[i].channel_id = i;
        tp->loaders[i].output_buffer = &tp->channel_buffers[i];
        tp->loaders[i].compressed_size = sizes[i];
        
        // Lê dados 4-bit brutos (SEM converter pra texto!)
        tp->loaders[i].compressed_4bit = malloc(sizes[i]);
        fseek(f, offsets[i], SEEK_SET);
        fread(tp->loaders[i].compressed_4bit, 1, sizes[i], f);
        
        pthread_create(&tp->loaders[i].thread, NULL, loader_thread_func, &tp->loaders[i]);
    }
    
    for (int i = 0; i < tp->header.channels; i++) {
        pthread_join(tp->loaders[i].thread, NULL);
        free(tp->loaders[i].compressed_4bit);
    }
    
    intercalar_canais_float(tp);
    
    for (int i = 0; i < tp->header.channels; i++) {
        free(tp->channel_buffers[i].data);
    }
    
    tp->running = 1;
    return tp;
}

void txacplay_close(txacplay_desc *tp) {
    if (!tp) return;
    tp->running = 0;
    if (tp->pcm_data) free(tp->pcm_data);
    if (tp->file) fclose(tp->file);
    free(tp);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Use: %s <arquivo.txac>\n", argv[0]);
        return 1;
    }
    
    printf("Starting TXAC Player v0.2.0...\n");
    txacplay_desc *tp = txacplay_open(argv[1]);
    if(!tp) {
        printf("Error opening file.\n");
        return 1;
    }

    saudio_setup(&(saudio_desc){
        .sample_rate = tp->header.sample_rate,
        .num_channels = tp->header.channels,
        .stream_userdata_cb = audio_cb,
        .user_data = tp,
        .buffer_frames = 4096
    });
    
    printf("Playing...\n Press:\n [space] to pause\n [x] to go back 5s\n [c] to go forward 5s\n [q] to exit\n\n");
    
    int wants_to_quit = 0;
    while (!wants_to_quit) {
        char c = getch();
        
        switch (c) {
            case ' ':
                toggle_pause(tp);
                break;
            case 'x': {
                double current = calculate_time(tp);
                txacplay_seek_absolute(tp, current - 5.0);
                break;
            }
            case 'c': {
                double current = calculate_time(tp);
                txacplay_seek_absolute(tp, current + 5.0);
                break;
            }
            case 'q': 
                wants_to_quit = 1; 
                break;
        }
        
        update_timer(tp);
    }
    
    saudio_shutdown();
    txacplay_close(tp);
    printf("\n\nBye bye\n");
    return 0;
}