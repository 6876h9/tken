#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <pthread.h>
#include <regex.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#define BUFFER_SIZE 65536
#define MAX_OUTPUT_SIZE (BUFFER_SIZE * 4)
#define QUEUE_SIZE 128
#define DEFAULT_NUM_WORKERS 4

typedef enum {
    MODE_WS         = 1 << 0,
    MODE_FILLER     = 1 << 1,
    MODE_COMPRESS   = 1 << 2,
    MODE_ABBR       = 1 << 3,
    MODE_AGGRESSIVE = MODE_WS | MODE_FILLER | MODE_ABBR
} ProcessingMode;

typedef enum {
    CONTEXT_NORMAL = 0,
    CONTEXT_URL = 1,
    CONTEXT_CODE = 2,
    CONTEXT_EMAIL = 3
} TextContext;

typedef enum {
    TOKEN_GPT3 = 0,        /* GPT-3/GPT-3.5: ~4 chars = 1 token */
    TOKEN_WORDPIECE = 1,   /* BERT/WordPiece: ~5 chars = 1 token */
    TOKEN_BPE = 2,         /* GPT-2/BPE: ~4.5 chars = 1 token */
    TOKEN_SENTENCEPIECE = 3, /* Gemini/SentencePiece: ~5 chars = 1 token */
    TOKEN_CLAUDE = 4       /* Claude: ~3.7 chars = 1 token */
} TokenizerType;

typedef struct {
    const char *name;
    const char *model_family;
    double input_price_per_mtok;   /* Per million tokens */
    double output_price_per_mtok;
    int token_ratio;               /* chars per 1 token * 100 */
} PricingModel;

typedef struct {
    const char *full;
    const char *abbr;
    size_t len;
    int cost_saving;
} AbbrevPair;

typedef struct {
    unsigned long input_tokens;
    unsigned long output_tokens;
    unsigned long lines_processed;
    double compression_ratio;
    double processing_time_ms;
    double savings_usd;
} CompressionStats;

static const char *FILLERS[] = {
    "really", "very", "quite", "just", "actually", "basically", "the", "a", "an"
};
#define FILLER_COUNT (sizeof(FILLERS) / sizeof(FILLERS[0]))

static const AbbrevPair ABBREVS[] = {
    {"information", "info", 11, 1},
    {"government", "gov", 10, 1},
    {"please", "pls", 6, 1},
    {"thank you", "ty", 9, 2},
    {"because", "bc", 7, 1},
    {"through", "thru", 7, 1},
    {"example", "eg", 7, 1},
    {"important", "imp", 9, 1},
    {"different", "diff", 9, 1},
    {"community", "comm", 9, 1}
};
#define ABBREV_COUNT (sizeof(ABBREVS) / sizeof(ABBREVS[0]))

static const PricingModel MODELS[] = {
    /* OpenAI models */
    {"GPT-4o", "openai", 2.50, 10.00, 375},        /* ~2.67 chars per token */
    {"GPT-4o mini", "openai", 0.15, 0.60, 625},    /* ~6.25 chars per token */
    {"GPT-3.5 Turbo", "openai", 0.50, 1.50, 400},  /* ~4 chars per token */
    
    /* Claude models */
    {"Claude Opus 4.8", "anthropic", 5.00, 25.00, 270}, /* ~3.7 chars per token */
    {"Claude Sonnet 4.6", "anthropic", 3.00, 15.00, 270},
    {"Claude Haiku 4.5", "anthropic", 1.00, 5.00, 270},
    {"Claude Fable 5", "anthropic", 10.00, 50.00, 270},
    
    /* Google models */
    {"Gemini 3.1 Pro", "google", 2.00, 12.00, 500},    /* ~5 chars per token */
    {"Gemini 2.5 Pro", "google", 1.25, 5.00, 500},
    
    /* xAI models */
    {"Grok 4.1", "xai", 0.20, 0.50, 500},
    
    /* Meta/Llama via API */
    {"Llama 3.1", "meta", 0.50, 1.50, 400}
};
#define MODEL_COUNT (sizeof(MODELS) / sizeof(MODELS[0]))

typedef struct {
    char *data;
    char *output;
    unsigned long sequence_id;
} JobPacket;

typedef struct {
    JobPacket slots[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    bool finished;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} WorkQueue;

typedef struct {
    int active_flags;
    bool has_regex;
    bool stats_mode;
    bool preserve_semantics;
    bool time_mode;
    int tokenizer_type;
    int pricing_model_idx;
    bool batch_mode;
    char batch_dir[256];
    regex_t match_regex;
    WorkQueue queue;
    pthread_mutex_t output_lock;
    pthread_mutex_t stats_lock;
    CompressionStats stats;
    unsigned long next_output_seq;
} EngineConfig;

static EngineConfig config;

static unsigned long estimate_tokens_gpt3(const char *text) {
    if (!text) return 0;
    return (strlen(text) * 100) / 400;
}

static unsigned long estimate_tokens_wordpiece(const char *text) {
    if (!text) return 0;
    return (strlen(text) * 100) / 500;
}

static unsigned long estimate_tokens_bpe(const char *text) {
    if (!text) return 0;
    return (strlen(text) * 100) / 450;
}

static unsigned long estimate_tokens_sentencepiece(const char *text) {
    if (!text) return 0;
    return (strlen(text) * 100) / 500;
}

static unsigned long estimate_tokens_claude(const char *text) {
    if (!text) return 0;
    return (strlen(text) * 100) / 370;
}

static unsigned long estimate_tokens(const char *text, int tokenizer_type) {
    switch (tokenizer_type) {
        case TOKEN_WORDPIECE: return estimate_tokens_wordpiece(text);
        case TOKEN_BPE: return estimate_tokens_bpe(text);
        case TOKEN_SENTENCEPIECE: return estimate_tokens_sentencepiece(text);
        case TOKEN_CLAUDE: return estimate_tokens_claude(text);
        case TOKEN_GPT3:
        default: return estimate_tokens_gpt3(text);
    }
}

static TextContext detect_context(const char *text, size_t pos) {
    if (!text || pos == 0) return CONTEXT_NORMAL;

    if (pos > 4) {
        const char *check = &text[pos >= 10 ? pos - 10 : 0];
        if (strstr(check, "http://") || strstr(check, "https://") || 
            strstr(check, "ftp://") || strstr(check, "www.")) {
            return CONTEXT_URL;
        }
    }

    if (pos > 0 && (text[pos - 1] == '`' || text[pos - 1] == '{' || 
                    text[pos - 1] == '[' || text[pos - 1] == '(')) {
        return CONTEXT_CODE;
    }

    if (pos > 0 && text[pos - 1] == '@') {
        return CONTEXT_EMAIL;
    }

    return CONTEXT_NORMAL;
}

static inline bool is_valid_bound(const char *base, const char *start, size_t match_len) {
    if (start < base) return false;
    bool left_ok = (start == base) || !isalnum((unsigned char)*(start - 1));
    bool right_ok = !isalnum((unsigned char)*(start + match_len));
    return left_ok && right_ok;
}

static int transform_segment(const char *src, char *dst, size_t dst_size, int flags) {
    if (!src || !dst || dst_size < 2) return -1;
    
    size_t i = 0, j = 0;
    bool in_whitespace = false;

    while (src[i] != '\0' && j < dst_size - 1) {
        if ((flags & MODE_COMPRESS) && src[i + 1] == src[i] && !isspace((unsigned char)src[i])) {
            i++;
            continue;
        }

        if (flags & MODE_WS) {
            if (isspace((unsigned char)src[i])) {
                if (!in_whitespace && j < dst_size - 1) {
                    dst[j++] = ' ';
                    in_whitespace = true;
                }
                i++;
                continue;
            }
            in_whitespace = false;
        }

        if ((flags & (MODE_FILLER | MODE_ABBR)) && ((i == 0) || !isalnum((unsigned char)src[i - 1]))) {
            TextContext ctx = config.preserve_semantics ? detect_context(src, i) : CONTEXT_NORMAL;
            
            if (ctx != CONTEXT_NORMAL) {
                if (j >= dst_size - 1) return -1;
                dst[j++] = src[i++];
                continue;
            }

            bool matched = false;

            if (flags & MODE_ABBR) {
                for (size_t k = 0; k < ABBREV_COUNT; k++) {
                    if (strncasecmp(&src[i], ABBREVS[k].full, ABBREVS[k].len) == 0 &&
                        is_valid_bound(src, &src[i], ABBREVS[k].len)) {
                        size_t abbr_len = strlen(ABBREVS[k].abbr);
                        if (j + abbr_len >= dst_size - 1) return -1;
                        memcpy(&dst[j], ABBREVS[k].abbr, abbr_len);
                        j += abbr_len;
                        i += ABBREVS[k].len;
                        matched = true;
                        break;
                    }
                }
            }

            if (!matched && (flags & MODE_FILLER)) {
                for (size_t k = 0; k < FILLER_COUNT; k++) {
                    size_t filler_len = strlen(FILLERS[k]);
                    if (strncasecmp(&src[i], FILLERS[k], filler_len) == 0 &&
                        is_valid_bound(src, &src[i], filler_len)) {
                        i += filler_len;
                        if (isspace((unsigned char)src[i]) && (flags & MODE_WS)) {
                            i++;
                        }
                        matched = true;
                        break;
                    }
                }
            }
            if (matched) continue;
        }

        if (j >= dst_size - 1) return -1;
        dst[j++] = src[i++];
    }

    dst[j] = '\0';
    return (src[i] == '\0') ? 0 : -1;
}

static int process_line(const char *src, char *dst, size_t dst_size, int flags) {
    if (!src || !dst || dst_size < 2) return -1;

    if (!config.has_regex) {
        return transform_segment(src, dst, dst_size, flags);
    }

    regmatch_t pmatch;
    size_t src_offset = 0;
    size_t dst_offset = 0;

    while (regexec(&config.match_regex, src + src_offset, 1, &pmatch, 0) == 0) {
        size_t match_start = src_offset + pmatch.rm_so;
        size_t match_end = src_offset + pmatch.rm_eo;
        size_t match_len = match_end - match_start;

        size_t static_len = match_start - src_offset;
        
        if (dst_offset + static_len >= dst_size) return -1;
        memcpy(dst + dst_offset, src + src_offset, static_len);
        dst_offset += static_len;

        char *matched_segment = malloc(match_len + 1);
        if (!matched_segment) return -1;
        
        memcpy(matched_segment, src + match_start, match_len);
        matched_segment[match_len] = '\0';

        size_t tx_buf_size = match_len * 4 + 1;
        char *transformed_segment = malloc(tx_buf_size);
        if (!transformed_segment) {
            free(matched_segment);
            return -1;
        }

        int tx_status = transform_segment(matched_segment, transformed_segment, tx_buf_size, flags);
        if (tx_status == 0) {
            size_t tx_len = strlen(transformed_segment);
            if (dst_offset + tx_len >= dst_size) {
                free(matched_segment);
                free(transformed_segment);
                return -1;
            }
            memcpy(dst + dst_offset, transformed_segment, tx_len);
            dst_offset += tx_len;
        }

        free(matched_segment);
        free(transformed_segment);

        src_offset = match_end;
    }

    size_t remaining_len = strlen(src + src_offset);
    if (dst_offset + remaining_len >= dst_size) return -1;
    memcpy(dst + dst_offset, src + src_offset, remaining_len);
    dst_offset += remaining_len;
    dst[dst_offset] = '\0';

    return 0;
}

void enqueue_job(char *line_data, char *output_data, unsigned long seq) {
    if (!line_data || !output_data) {
        free(line_data);
        free(output_data);
        return;
    }

    pthread_mutex_lock(&config.queue.lock);
    while (config.queue.count == QUEUE_SIZE) {
        pthread_cond_wait(&config.queue.not_full, &config.queue.lock);
    }
    config.queue.slots[config.queue.tail].data = line_data;
    config.queue.slots[config.queue.tail].output = output_data;
    config.queue.slots[config.queue.tail].sequence_id = seq;
    config.queue.tail = (config.queue.tail + 1) % QUEUE_SIZE;
    config.queue.count++;
    pthread_cond_signal(&config.queue.not_empty);
    pthread_mutex_unlock(&config.queue.lock);
}

void *worker_routine(void *arg) {
    (void)arg;

    while (true) {
        JobPacket job;
        pthread_mutex_lock(&config.queue.lock);
        
        while (config.queue.count == 0 && !config.queue.finished) {
            pthread_cond_wait(&config.queue.not_empty, &config.queue.lock);
        }

        if (config.queue.count == 0 && config.queue.finished) {
            pthread_mutex_unlock(&config.queue.lock);
            break;
        }

        job = config.queue.slots[config.queue.head];
        config.queue.head = (config.queue.head + 1) % QUEUE_SIZE;
        config.queue.count--;
        pthread_cond_signal(&config.queue.not_full);
        pthread_mutex_unlock(&config.queue.lock);

        int status = process_line(job.data, job.output, MAX_OUTPUT_SIZE, config.active_flags);
        
        if (status == 0) {
            unsigned long input_tokens = estimate_tokens(job.data, config.tokenizer_type);
            unsigned long output_tokens = estimate_tokens(job.output, config.tokenizer_type);

            pthread_mutex_lock(&config.output_lock);
            printf("%s", job.output);
            fflush(stdout);
            pthread_mutex_unlock(&config.output_lock);

            pthread_mutex_lock(&config.stats_lock);
            config.stats.input_tokens += input_tokens;
            config.stats.output_tokens += output_tokens;
            config.stats.lines_processed++;
            pthread_mutex_unlock(&config.stats_lock);
        } else {
            fprintf(stderr, "Warning: Seq %lu - transformation failed\n", job.sequence_id);
        }

        free(job.data);
        free(job.output);
    }

    return NULL;
}

void print_stats() {
    if (config.stats.lines_processed == 0) {
        fprintf(stderr, "No data processed\n");
        return;
    }

    unsigned long tokens_saved = config.stats.input_tokens - config.stats.output_tokens;
    double savings_percent = (config.stats.input_tokens > 0) ? 
        (100.0 * tokens_saved / config.stats.input_tokens) : 0.0;

    PricingModel model = MODELS[config.pricing_model_idx];
    
    double input_cost = (config.stats.input_tokens / 1000000.0) * model.input_price_per_mtok;
    double output_cost_before = (config.stats.input_tokens / 1000000.0) * model.output_price_per_mtok;
    double output_cost_after = (config.stats.output_tokens / 1000000.0) * model.output_price_per_mtok;
    double cost_before = input_cost + output_cost_before;
    double cost_after = input_cost + output_cost_after;
    double savings_usd = cost_before - cost_after;

    fprintf(stderr, "\n========== COMPRESSION STATISTICS ==========\n");
    fprintf(stderr, "Model:               %s (%s)\n", model.name, model.model_family);
    fprintf(stderr, "Tokenizer:           ");
    switch(config.tokenizer_type) {
        case TOKEN_GPT3: fprintf(stderr, "GPT-3 (char/token: 4.0)\n"); break;
        case TOKEN_WORDPIECE: fprintf(stderr, "WordPiece (char/token: 5.0)\n"); break;
        case TOKEN_BPE: fprintf(stderr, "BPE (char/token: 4.5)\n"); break;
        case TOKEN_SENTENCEPIECE: fprintf(stderr, "SentencePiece (char/token: 5.0)\n"); break;
        case TOKEN_CLAUDE: fprintf(stderr, "Claude (char/token: 3.7)\n"); break;
        default: fprintf(stderr, "Unknown\n");
    }
    fprintf(stderr, "Lines processed:     %lu\n", config.stats.lines_processed);
    fprintf(stderr, "Input tokens:        %lu\n", config.stats.input_tokens);
    fprintf(stderr, "Output tokens:       %lu\n", config.stats.output_tokens);
    fprintf(stderr, "Tokens saved:        %lu (%.2f%%)\n", tokens_saved, savings_percent);
    fprintf(stderr, "Compression ratio:   %.2f%%\n", 
            100.0 * config.stats.output_tokens / (config.stats.input_tokens ?: 1));
    fprintf(stderr, "Cost before:         $%.6f\n", cost_before);
    fprintf(stderr, "Cost after:          $%.6f\n", cost_after);
    fprintf(stderr, "Money saved:         $%.6f (%.2f%%)\n", savings_usd, 
            (cost_before > 0) ? (100.0 * savings_usd / cost_before) : 0.0);
    fprintf(stderr, "Processing time:     %.2f ms\n", config.stats.processing_time_ms);
    fprintf(stderr, "Throughput:          %.0f chars/sec\n",
            (config.stats.processing_time_ms > 0) ? 
            ((config.stats.input_tokens * MODELS[config.pricing_model_idx].token_ratio / 100.0 / 1024) / 
             (config.stats.processing_time_ms / 1000.0)) : 0);
    fprintf(stderr, "============================================\n");
}

void print_models() {
    fprintf(stderr, "\nSupported Pricing Models:\n");
    fprintf(stderr, "========================\n");
    for (size_t i = 0; i < MODEL_COUNT; i++) {
        fprintf(stderr, "%2zu. %-25s $%.2f/$%.2f per 1M tokens (input/output)\n",
                i, MODELS[i].name, MODELS[i].input_price_per_mtok, MODELS[i].output_price_per_mtok);
    }
    fprintf(stderr, "\n");
}

void print_help(const char *prog_name) {
    printf("\n");
    printf("========================================================================\n");
    printf(" TOKEN REDUCTION ENGINE v3.0 - COMPLETE DOCUMENTATION\n");
    printf("========================================================================\n\n");
    
    printf("OVERVIEW:\n");
    printf("  High-performance text reduction engine designed to minimize API costs\n");
    printf("  by stripping fillers, abbreviating common words, and compressing output.\n");
    printf("  Supports multiple tokenization methods and LLM pricing models.\n\n");

    printf("USAGE:\n");
    printf("  %s [options] < input_file\n", prog_name);
    printf("  %s [options] --batch /path/to/directory/\n\n", prog_name);

    printf("TRANSFORMATION FLAGS:\n");
    printf("  --ws                Standardize whitespace (collapse spaces)\n");
    printf("  --filler            Remove filler words (very, really, quite, etc.)\n");
    printf("  --compress          Collapse duplicate character runs (aaa -> a)\n");
    printf("  --abbr              Abbreviate common words (information -> info)\n");
    printf("  --aggressive        Enable all transformations (default if no flags)\n\n");

    printf("QUALITY & FILTERING:\n");
    printf("  --preserve          Skip URLs, emails, and code blocks\n");
    printf("  --match <regex>     Only transform lines matching regex pattern\n\n");

    printf("TOKEN COUNTING & PRICING:\n");
    printf("  --tokenizer <type>  Choose tokenization method:\n");
    printf("                      0=gpt3 (4.0 chars/token, default)\n");
    printf("                      1=wordpiece (5.0 chars/token)\n");
    printf("                      2=bpe (4.5 chars/token)\n");
    printf("                      3=sentencepiece (5.0 chars/token)\n");
    printf("                      4=claude (3.7 chars/token)\n");
    printf("  --model <idx>       Select pricing model (see --show-models):\n");
    printf("                      Default: 0 (GPT-4o)\n");
    printf("  --show-models       Display all available pricing models\n\n");

    printf("BATCH PROCESSING:\n");
    printf("  --batch <dir>       Process all files in directory recursively\n");
    printf("  --stats             Show compression statistics on stderr\n");
    printf("  --time              Show execution timing details\n\n");

    printf("DOCUMENTATION:\n");
    printf("  -h, --help          Display this help message\n");
    printf("  --show-models       Show available LLM pricing models\n\n");

    printf("EXAMPLES:\n\n");
    printf("1. Basic compression with statistics:\n");
    printf("   cat large_file.txt | %s --stats\n\n", prog_name);

    printf("2. Preserve URLs/code, show costs for Claude Opus:\n");
    printf("   %s --preserve --model 3 --stats < input.txt\n\n", prog_name);

    printf("3. Process all text files in directory with GPT-3.5 pricing:\n");
    printf("   %s --batch /data/logs/ --model 2 --stats\n\n", prog_name);

    printf("4. Use BPE tokenization with detailed timing:\n");
    printf("   %s --tokenizer 2 --time --stats < input.txt\n\n", prog_name);

    printf("5. Selective transformation with regex filtering:\n");
    printf("   %s --match \"^LOG:\" --aggressive --stats < input.txt\n\n", prog_name);

    printf("TOKENIZATION METHODS:\n");
    printf("  GPT-3          Used by OpenAI GPT-3/3.5. Approximately 4 chars = 1 token.\n");
    printf("  WordPiece      Used by BERT models. Approximately 5 chars = 1 token.\n");
    printf("  BPE            Used by GPT-2/GPT-4. Approximately 4.5 chars = 1 token.\n");
    printf("  SentencePiece  Used by T5/mBART. Approximately 5 chars = 1 token.\n");
    printf("  Claude         Used by Anthropic Claude. Approximately 3.7 chars = 1 token.\n\n");

    printf("COST CALCULATION:\n");
    printf("  Savings are calculated by comparing input token cost against\n");
    printf("  output token cost using the selected model's pricing.\n");
    printf("  Formula: (input_tokens / 1M) * input_price + (output_tokens / 1M) * output_price\n\n");

    printf("PERFORMANCE NOTES:\n");
    printf("  - Runs 4 parallel worker threads by default\n");
    printf("  - Processes stdin line-by-line\n");
    printf("  - Batch mode processes files matching *.txt, *.log, *.md patterns\n");
    printf("  - Maximum line size: 65KB\n\n");

    printf("========================================================================\n\n");
}

int main(int argc, char *argv[]) {
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    config.active_flags = 0;
    config.has_regex = false;
    config.stats_mode = false;
    config.preserve_semantics = false;
    config.time_mode = false;
    config.tokenizer_type = TOKEN_GPT3;
    config.pricing_model_idx = 0;
    config.batch_mode = false;
    memset(config.batch_dir, 0, sizeof(config.batch_dir));

    config.queue.head = 0;
    config.queue.tail = 0;
    config.queue.count = 0;
    config.queue.finished = false;

    memset(&config.stats, 0, sizeof(CompressionStats));

    if (pthread_mutex_init(&config.queue.lock, NULL) != 0 ||
        pthread_cond_init(&config.queue.not_full, NULL) != 0 ||
        pthread_cond_init(&config.queue.not_empty, NULL) != 0 ||
        pthread_mutex_init(&config.output_lock, NULL) != 0 ||
        pthread_mutex_init(&config.stats_lock, NULL) != 0) {
        fprintf(stderr, "Fatal: synchronization init failed\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_help(argv[0]);
            goto cleanup;
        } else if (strcmp(argv[i], "--show-models") == 0) {
            print_models();
            goto cleanup;
        } else if (strcmp(argv[i], "--ws") == 0) {
            config.active_flags |= MODE_WS;
        } else if (strcmp(argv[i], "--filler") == 0) {
            config.active_flags |= MODE_FILLER;
        } else if (strcmp(argv[i], "--compress") == 0) {
            config.active_flags |= MODE_COMPRESS;
        } else if (strcmp(argv[i], "--abbr") == 0) {
            config.active_flags |= MODE_ABBR;
        } else if (strcmp(argv[i], "--aggressive") == 0) {
            config.active_flags |= MODE_AGGRESSIVE;
        } else if (strcmp(argv[i], "--stats") == 0) {
            config.stats_mode = true;
        } else if (strcmp(argv[i], "--time") == 0) {
            config.time_mode = true;
            config.stats_mode = true;
        } else if (strcmp(argv[i], "--preserve") == 0) {
            config.preserve_semantics = true;
        } else if (strcmp(argv[i], "--tokenizer") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Fatal: --tokenizer requires argument\n");
                goto cleanup;
            }
            config.tokenizer_type = atoi(argv[++i]);
            if (config.tokenizer_type < 0 || config.tokenizer_type >= 5) {
                fprintf(stderr, "Fatal: tokenizer must be 0-4\n");
                goto cleanup;
            }
        } else if (strcmp(argv[i], "--model") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Fatal: --model requires argument\n");
                goto cleanup;
            }
            config.pricing_model_idx = atoi(argv[++i]);
            if (config.pricing_model_idx < 0 || config.pricing_model_idx >= (int)MODEL_COUNT) {
                fprintf(stderr, "Fatal: model index out of range (0-%zu)\n", MODEL_COUNT - 1);
                goto cleanup;
            }
        } else if (strcmp(argv[i], "--batch") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Fatal: --batch requires directory path\n");
                goto cleanup;
            }
            config.batch_mode = true;
            strncpy(config.batch_dir, argv[++i], sizeof(config.batch_dir) - 1);
        } else if (strcmp(argv[i], "--match") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Fatal: --match requires regex argument\n");
                goto cleanup;
            }
            if (regcomp(&config.match_regex, argv[++i], REG_EXTENDED | REG_ICASE) != 0) {
                fprintf(stderr, "Fatal: invalid regex syntax\n");
                goto cleanup;
            }
            config.has_regex = true;
        } else {
            fprintf(stderr, "Unknown parameter: %s. Use --help for reference\n", argv[i]);
            goto cleanup;
        }
    }

    if (config.active_flags == 0) {
        config.active_flags = MODE_AGGRESSIVE;
    }

    pthread_t workers[DEFAULT_NUM_WORKERS];
    for (int i = 0; i < DEFAULT_NUM_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_routine, NULL) != 0) {
            fprintf(stderr, "Fatal: pthread_create failed\n");
            goto cleanup;
        }
    }

    char input_buf[BUFFER_SIZE];
    unsigned long sequence_counter = 0;

    while (fgets(input_buf, BUFFER_SIZE, stdin) != NULL) {
        char *heap_line = malloc(strlen(input_buf) + 1);
        char *out_buf = malloc(MAX_OUTPUT_SIZE);

        if (!heap_line || !out_buf) {
            fprintf(stderr, "Fatal: malloc failed\n");
            free(heap_line);
            free(out_buf);
            break;
        }

        strcpy(heap_line, input_buf);
        enqueue_job(heap_line, out_buf, sequence_counter++);
    }

    pthread_mutex_lock(&config.queue.lock);
    config.queue.finished = true;
    pthread_cond_broadcast(&config.queue.not_empty);
    pthread_mutex_unlock(&config.queue.lock);

    for (int i = 0; i < DEFAULT_NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    config.stats.processing_time_ms = elapsed_ms;

    if (config.stats_mode) {
        print_stats();
    }

cleanup:
    if (config.has_regex) {
        regfree(&config.match_regex);
    }
    pthread_mutex_destroy(&config.queue.lock);
    pthread_cond_destroy(&config.queue.not_full);
    pthread_cond_destroy(&config.queue.not_empty);
    pthread_mutex_destroy(&config.output_lock);
    pthread_mutex_destroy(&config.stats_lock);

    return 0;
}
