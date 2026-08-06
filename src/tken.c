#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <pthread.h>
#include <regex.h>
#include <unistd.h>

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

typedef struct {
    const char *full;
    const char *abbr;
    size_t len;
} AbbrevPair;

static const char *FILLERS[] = {
    "really", "very", "quite", "just", "actually", "basically", "the", "a", "an"
};
#define FILLER_COUNT (sizeof(FILLERS) / sizeof(FILLERS[0]))

static const AbbrevPair ABBREVS[] = {
    {"information", "info", 11},
    {"government", "gov", 10},
    {"please", "pls", 6},
    {"thank you", "ty", 9},
    {"because", "bc", 7},
    {"through", "thru", 7},
    {"example", "eg", 7},
    {"important", "imp", 9},
    {"different", "diff", 9},
    {"community", "comm", 9}
};
#define ABBREV_COUNT (sizeof(ABBREVS) / sizeof(ABBREVS[0]))

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
    regex_t match_regex;
    WorkQueue queue;
    pthread_mutex_t output_lock;
    unsigned long next_output_seq;
} EngineConfig;

static EngineConfig config;

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
        /* Duplicate compression */
        if ((flags & MODE_COMPRESS) && src[i + 1] == src[i] && !isspace((unsigned char)src[i])) {
            i++;
            continue;
        }

        /* Whitespace normalization */
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

        /* Filler + abbreviation removal */
        if ((flags & (MODE_FILLER | MODE_ABBR)) && ((i == 0) || !isalnum((unsigned char)src[i - 1]))) {
            bool matched = false;

            if (flags & MODE_ABBR) {
                for (size_t k = 0; k < ABBREV_COUNT; k++) {
                    if (strncasecmp(&src[i], ABBREVS[k].full, ABBREVS[k].len) == 0 &&
                        is_valid_bound(src, &src[i], ABBREVS[k].len)) {
                        size_t abbr_len = strlen(ABBREVS[k].abbr);
                        if (j + abbr_len >= dst_size - 1) return -1; /* Buffer overflow */
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

        if (j >= dst_size - 1) return -1; /* Buffer full */
        dst[j++] = src[i++];
    }

    dst[j] = '\0';
    return (src[i] == '\0') ? 0 : -1; /* -1 if input truncated */
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
            pthread_mutex_lock(&config.output_lock);
            printf("%s", job.output);
            fflush(stdout);
            pthread_mutex_unlock(&config.output_lock);
        } else {
            fprintf(stderr, "Warning: Seq %lu - line transformation failed (buffer exhausted)\n", job.sequence_id);
        }

        free(job.data);
        free(job.output);
    }

    return NULL;
}

void print_usage(const char *prog_name) {
    printf("========================================================================\n");
    printf(" TOKEN REDUCTION ENGINE & STREAM TRANSFORMER (PRODUCTION BUILD)\n");
    printf("========================================================================\n");
    printf("Usage: %s [options] < input_text\n\n", prog_name);
    printf("Operational Mode Configuration Flags:\n");
    printf("  -h, --help    Display this technical documentation\n");
    printf("  --ws          Standardize whitespace; compress redundant spaces\n");
    printf("  --filler      Strip context filler tokens using word boundaries\n");
    printf("  --compress    Collapse duplicate alphanumeric character runs\n");
    printf("  --abbr        Translate explicit matches into standardized shortcuts\n");
    printf("  --aggressive  Engage all reduction strategies (WS, filler, abbreviations)\n");
    printf("                [Default if no flags specified]\n\n");
    printf("Performance & Scope Tuning Options:\n");
    printf("  --match <re>  Filter pipeline. Only mutate sub-blocks matching regex.\n\n");
    printf("Execution Examples:\n");
    printf("  1. Basic compression:\n");
    printf("     echo \"This   is   very   important\" | %s\n\n", prog_name);
    printf("  2. Regex sandboxing:\n");
    printf("     echo \"LOG: please process this\" | %s --match \"LOG:.*\"\n", prog_name);
    printf("========================================================================\n");
}

int main(int argc, char *argv[]) {
    config.active_flags = 0;
    config.has_regex = false;
    config.next_output_seq = 0;

    config.queue.head = 0;
    config.queue.tail = 0;
    config.queue.count = 0;
    config.queue.finished = false;

    if (pthread_mutex_init(&config.queue.lock, NULL) != 0 ||
        pthread_cond_init(&config.queue.not_full, NULL) != 0 ||
        pthread_cond_init(&config.queue.not_empty, NULL) != 0 ||
        pthread_mutex_init(&config.output_lock, NULL) != 0) {
        fprintf(stderr, "Fatal: Failed to initialize synchronization primitives\n");
        return 1;
    }

    /* Argument parsing */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            pthread_mutex_destroy(&config.queue.lock);
            pthread_cond_destroy(&config.queue.not_full);
            pthread_cond_destroy(&config.queue.not_empty);
            pthread_mutex_destroy(&config.output_lock);
            return 0;
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
        } else if (strcmp(argv[i], "--match") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Fatal: --match requires a regex argument\n");
                pthread_mutex_destroy(&config.queue.lock);
                pthread_cond_destroy(&config.queue.not_full);
                pthread_cond_destroy(&config.queue.not_empty);
                pthread_mutex_destroy(&config.output_lock);
                return 1;
            }
            if (regcomp(&config.match_regex, argv[++i], REG_EXTENDED | REG_ICASE) != 0) {
                fprintf(stderr, "Fatal: Invalid regex syntax\n");
                pthread_mutex_destroy(&config.queue.lock);
                pthread_cond_destroy(&config.queue.not_full);
                pthread_cond_destroy(&config.queue.not_empty);
                pthread_mutex_destroy(&config.output_lock);
                return 1;
            }
            config.has_regex = true;
        } else {
            fprintf(stderr, "Unknown parameter: %s. Use --help for reference\n", argv[i]);
            pthread_mutex_destroy(&config.queue.lock);
            pthread_cond_destroy(&config.queue.not_full);
            pthread_cond_destroy(&config.queue.not_empty);
            pthread_mutex_destroy(&config.output_lock);
            return 1;
        }
    }

    if (config.active_flags == 0) {
        config.active_flags = MODE_AGGRESSIVE;
    }

    /* Spawn workers */
    pthread_t workers[DEFAULT_NUM_WORKERS];
    for (int i = 0; i < DEFAULT_NUM_WORKERS; i++) {
        if (pthread_create(&workers[i], NULL, worker_routine, NULL) != 0) {
            fprintf(stderr, "Fatal: Failed to create worker thread %d\n", i);
            return 1;
        }
    }

    char input_buf[BUFFER_SIZE];
    unsigned long sequence_counter = 0;

    /* Main ingestion loop */
    while (fgets(input_buf, BUFFER_SIZE, stdin) != NULL) {
        char *heap_line = malloc(strlen(input_buf) + 1);
        char *out_buf = malloc(MAX_OUTPUT_SIZE);

        if (!heap_line || !out_buf) {
            fprintf(stderr, "Fatal: Memory allocation failed\n");
            free(heap_line);
            free(out_buf);
            break;
        }

        strcpy(heap_line, input_buf);
        enqueue_job(heap_line, out_buf, sequence_counter++);
    }

    /* Signal shutdown */
    pthread_mutex_lock(&config.queue.lock);
    config.queue.finished = true;
    pthread_cond_broadcast(&config.queue.not_empty);
    pthread_mutex_unlock(&config.queue.lock);

    /* Wait for workers */
    for (int i = 0; i < DEFAULT_NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }

    /* Cleanup */
    if (config.has_regex) {
        regfree(&config.match_regex);
    }
    pthread_mutex_destroy(&config.queue.lock);
    pthread_cond_destroy(&config.queue.not_full);
    pthread_cond_destroy(&config.queue.not_empty);
    pthread_mutex_destroy(&config.output_lock);

    return 0;
}
