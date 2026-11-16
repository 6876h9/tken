#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef USE_CUDA
#include <cuda_runtime.h>
#endif

#ifdef __CUDACC__
#define HD __host__ __device__
#else
#define HD
#endif

#define VERSION          "4.0"
#define BATCH_MAX_LINES  8192
#define BATCH_MAX_BYTES  (8u * 1024u * 1024u)
#define GPU_MIN_BYTES    (4u * 1024u * 1024u)
#define MAX_THREADS      64
#define DEFAULT_THREADS  4
#define PATH_BUF         4096
#define MAX_RULES        16
#define RULE_TEXT        24

enum {
    MODE_WS         = 1u << 0,
    MODE_FILLER     = 1u << 1,
    MODE_COMPRESS   = 1u << 2,
    MODE_ABBR       = 1u << 3,
    MODE_AGGRESSIVE = MODE_WS | MODE_FILLER | MODE_ABBR
};

typedef struct {
    char full[RULE_TEXT];
    char abbr[RULE_TEXT];
    unsigned char full_len;
    unsigned char abbr_len;
} Rule;

typedef struct {
    Rule abbrs[MAX_RULES];
    Rule fillers[MAX_RULES];
    int n_abbrs;
    int n_fillers;
} RuleSet;

HD static inline int ch_is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

HD static inline int ch_is_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

HD static inline unsigned char ch_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + 32) : c;
}

HD static inline int is_decor(unsigned char c) {
    return c == '-' || c == '=' || c == '_' || c == '*' || c == '#' || c == '~';
}

HD static inline int match_ci(const char *s, size_t n, size_t pos,
                              const char *word, size_t wlen) {
    if (pos + wlen > n) return 0;
    for (size_t k = 0; k < wlen; k++) {
        if (ch_lower((unsigned char)s[pos + k]) != (unsigned char)word[k]) return 0;
    }
    return 1;
}

HD static inline int bound_ok(const char *s, size_t n, size_t pos, size_t len) {
    if (pos > 0 && ch_is_alnum((unsigned char)s[pos - 1])) return 0;
    if (pos + len < n && ch_is_alnum((unsigned char)s[pos + len])) return 0;
    return 1;
}

HD static inline int token_is_special(const char *s, size_t start, size_t end) {
    if (end - start >= 4 &&
        ch_lower((unsigned char)s[start]) == 'w' &&
        ch_lower((unsigned char)s[start + 1]) == 'w' &&
        ch_lower((unsigned char)s[start + 2]) == 'w' &&
        s[start + 3] == '.') {
        return 1;
    }
    for (size_t k = start; k < end; k++) {
        if (s[k] == '@') return 1;
        if (s[k] == ':' && k + 2 < end && s[k + 1] == '/' && s[k + 2] == '/') return 1;
    }
    return 0;
}

HD static inline size_t transform_line(const char *src, size_t n, char *dst,
                                       unsigned flags, int preserve,
                                       const RuleSet *rs) {
    size_t i = 0, o = 0;
    int pending = 0;
    int emitted = 0;
    int in_code = 0;

#define FLUSH() do { if (pending && emitted) dst[o++] = ' '; pending = 0; } while (0)

    while (i < n) {
        unsigned char c = (unsigned char)src[i];

        if (in_code) {
            if (c == '`') in_code = 0;
            dst[o++] = (char)c;
            i++;
            continue;
        }

        if (preserve && c == '`') {
            FLUSH();
            in_code = 1;
            dst[o++] = '`';
            emitted = 1;
            i++;
            continue;
        }

        if (ch_is_space(c)) {
            if ((flags & MODE_WS) && emitted) {
                pending = 1;
            } else {
                dst[o++] = (char)c;
            }
            i++;
            continue;
        }

        if (preserve && (i == 0 || ch_is_space((unsigned char)src[i - 1]))) {
            size_t end = i;
            while (end < n && !ch_is_space((unsigned char)src[end])) end++;
            if (token_is_special(src, i, end)) {
                FLUSH();
                memcpy(dst + o, src + i, end - i);
                o += end - i;
                i = end;
                emitted = 1;
                continue;
            }
        }

        if ((flags & MODE_COMPRESS) && is_decor(c)) {
            size_t run = 1;
            while (i + run < n && (unsigned char)src[i + run] == c) run++;
            size_t keep = run > 3 ? 3 : run;
            FLUSH();
            for (size_t k = 0; k < keep; k++) dst[o++] = (char)c;
            emitted = 1;
            i += run;
            continue;
        }

        if ((flags & (MODE_ABBR | MODE_FILLER)) && ch_is_alnum(c) &&
            (i == 0 || !ch_is_alnum((unsigned char)src[i - 1]))) {
            int done = 0;

            if (flags & MODE_ABBR) {
                for (int k = 0; k < rs->n_abbrs && !done; k++) {
                    const Rule *r = &rs->abbrs[k];
                    if (match_ci(src, n, i, r->full, r->full_len) &&
                        bound_ok(src, n, i, r->full_len)) {
                        FLUSH();
                        for (unsigned m = 0; m < r->abbr_len; m++) dst[o++] = r->abbr[m];
                        i += r->full_len;
                        emitted = 1;
                        done = 1;
                    }
                }
            }

            if (!done && (flags & MODE_FILLER)) {
                for (int k = 0; k < rs->n_fillers && !done; k++) {
                    const Rule *r = &rs->fillers[k];
                    if (match_ci(src, n, i, r->full, r->full_len) &&
                        bound_ok(src, n, i, r->full_len)) {
                        i += r->full_len;
                        if (!(flags & MODE_WS) && i < n && (src[i] == ' ' || src[i] == '\t')) i++;
                        done = 1;
                    }
                }
            }

            if (done) continue;
        }

        FLUSH();
        dst[o++] = (char)c;
        emitted = 1;
        i++;
    }

#undef FLUSH
    return o;
}

static void set_rule(Rule *r, const char *full, const char *abbr) {
    size_t fl = strlen(full);
    size_t al = abbr ? strlen(abbr) : 0;
    memset(r, 0, sizeof *r);
    memcpy(r->full, full, fl);
    r->full_len = (unsigned char)fl;
    if (abbr) {
        memcpy(r->abbr, abbr, al);
        r->abbr_len = (unsigned char)al;
    }
}

static void ruleset_init(RuleSet *rs) {
    static const char *const abbr_pairs[][2] = {
        {"information", "info"}, {"government", "gov"}, {"please", "pls"},
        {"thank you", "ty"}, {"because", "bc"}, {"through", "thru"},
        {"example", "eg"}, {"important", "imp"}, {"different", "diff"},
        {"community", "comm"}
    };
    static const char *const fillers[] = {
        "really", "very", "quite", "just", "actually", "basically"
    };

    memset(rs, 0, sizeof *rs);
    for (size_t k = 0; k < sizeof abbr_pairs / sizeof abbr_pairs[0] && k < MAX_RULES; k++) {
        set_rule(&rs->abbrs[k], abbr_pairs[k][0], abbr_pairs[k][1]);
        rs->n_abbrs++;
    }
    for (size_t k = 0; k < sizeof fillers / sizeof fillers[0] && k < MAX_RULES; k++) {
        set_rule(&rs->fillers[k], fillers[k], NULL);
        rs->n_fillers++;
    }
}

typedef enum { BACKEND_CPU = 0, BACKEND_GPU = 1, BACKEND_AUTO = 2 } Backend;

typedef enum {
    TOKEN_GPT3, TOKEN_WORDPIECE, TOKEN_BPE, TOKEN_SENTENCEPIECE, TOKEN_CLAUDE, TOKEN_COUNT
} TokenizerType;

static const char *const TOKENIZER_NAMES[TOKEN_COUNT] = {
    "GPT-3 (4.0 chars/token)", "WordPiece (5.0 chars/token)", "BPE (4.5 chars/token)",
    "SentencePiece (5.0 chars/token)", "Claude (3.7 chars/token)"
};
static const double CHARS_PER_TOKEN[TOKEN_COUNT] = {4.0, 5.0, 4.5, 5.0, 3.7};

typedef struct {
    const char *name;
    const char *family;
    double input_per_mtok;
    double output_per_mtok;
} PricingModel;

static const PricingModel MODELS[] = {
    {"GPT-4o",            "openai",    2.50, 10.00},
    {"GPT-4o mini",       "openai",    0.15,  0.60},
    {"GPT-3.5 Turbo",     "openai",    0.50,  1.50},
    {"Claude Opus 4.8",   "anthropic", 5.00, 25.00},
    {"Claude Sonnet 4.6", "anthropic", 3.00, 15.00},
    {"Claude Haiku 4.5",  "anthropic", 1.00,  5.00},
    {"Claude Fable 5",    "anthropic",10.00, 50.00},
    {"Gemini 3.1 Pro",    "google",    2.00, 12.00},
    {"Gemini 2.5 Pro",    "google",    1.25,  5.00},
    {"Grok 4.1",          "xai",       0.20,  0.50},
    {"Llama 3.1",         "meta",      0.50,  1.50},
};
#define MODEL_COUNT (sizeof MODELS / sizeof MODELS[0])

typedef struct {
    char *in;
    char *out;
    size_t *off;
    size_t *len;
    size_t *out_len;
    unsigned char *mask;
    size_t count;
    size_t bytes;
    size_t cap_bytes;
} Batch;

typedef struct {
    unsigned flags;
    int preserve;
    RuleSet rules;

    int has_regex;
    regex_t re;

    Backend backend;
    int threads;
    int gpu_ready;

    int tokenizer;
    int model_idx;
    int stats_on;
    int time_on;

    unsigned long long in_bytes;
    unsigned long long out_bytes;
    unsigned long long lines;
    unsigned long gpu_batches;
    unsigned long cpu_batches;
    double tx_ms;
} Ctx;

static void die(const char *msg) {
    fprintf(stderr, "Fatal: %s\n", msg);
    exit(1);
}

static int batch_init(Batch *b) {
    memset(b, 0, sizeof *b);
    b->off     = (size_t *)malloc(BATCH_MAX_LINES * sizeof(size_t));
    b->len     = (size_t *)malloc(BATCH_MAX_LINES * sizeof(size_t));
    b->out_len = (size_t *)malloc(BATCH_MAX_LINES * sizeof(size_t));
    b->mask    = (unsigned char *)malloc(BATCH_MAX_LINES);
    b->cap_bytes = 1u << 20;
    b->in  = (char *)malloc(b->cap_bytes);
    b->out = (char *)malloc(b->cap_bytes);
    if (!b->off || !b->len || !b->out_len || !b->mask || !b->in || !b->out) return -1;
    return 0;
}

static void batch_free(Batch *b) {
    free(b->in);
    free(b->out);
    free(b->off);
    free(b->len);
    free(b->out_len);
    free(b->mask);
    memset(b, 0, sizeof *b);
}

static int batch_grow(Batch *b, size_t need) {
    if (need <= b->cap_bytes) return 0;
    size_t nc = b->cap_bytes;
    while (nc < need) nc *= 2;
    char *ni = (char *)realloc(b->in, nc);
    if (!ni) return -1;
    b->in = ni;
    char *no = (char *)realloc(b->out, nc);
    if (!no) return -1;
    b->out = no;
    b->cap_bytes = nc;
    return 0;
}

static size_t read_batch(FILE *fp, Batch *b) {
    static char *line = NULL;
    static size_t cap = 0;
    b->count = 0;
    b->bytes = 0;

    while (b->count < BATCH_MAX_LINES && b->bytes < BATCH_MAX_BYTES) {
        ssize_t r = getline(&line, &cap, fp);
        if (r < 0) break;
        size_t n = (size_t)r;
        if (n && line[n - 1] == '\n') n--;
        if (n && line[n - 1] == '\r') n--;

        if (batch_grow(b, b->bytes + n + 1) != 0) die("out of memory");
        memcpy(b->in + b->bytes, line, n);
        b->in[b->bytes + n] = '\0';
        b->off[b->count] = b->bytes;
        b->len[b->count] = n;
        b->bytes += n + 1;
        b->count++;
    }
    return b->count;
}

static void compute_mask(Ctx *c, Batch *b) {
    for (size_t i = 0; i < b->count; i++) {
        if (!c->has_regex) {
            b->mask[i] = 1;
        } else {
            b->mask[i] = (regexec(&c->re, b->in + b->off[i], 0, NULL, 0) == 0);
        }
    }
}

typedef struct {
    Ctx *c;
    Batch *b;
    size_t start;
    size_t end;
} CpuSlice;

static void *cpu_slice(void *arg) {
    CpuSlice *s = (CpuSlice *)arg;
    Batch *b = s->b;
    for (size_t i = s->start; i < s->end; i++) {
        if (!b->mask[i]) {
            memcpy(b->out + b->off[i], b->in + b->off[i], b->len[i]);
            b->out_len[i] = b->len[i];
        } else {
            b->out_len[i] = transform_line(b->in + b->off[i], b->len[i],
                                           b->out + b->off[i], s->c->flags,
                                           s->c->preserve, &s->c->rules);
        }
    }
    return NULL;
}

static void transform_cpu(Ctx *c, Batch *b) {
    size_t nt = (size_t)c->threads;
    if (nt > b->count) nt = b->count;
    if (nt <= 1) {
        CpuSlice s = {c, b, 0, b->count};
        cpu_slice(&s);
        return;
    }

    pthread_t th[MAX_THREADS];
    CpuSlice sl[MAX_THREADS];
    size_t chunk = (b->count + nt - 1) / nt;

    for (size_t t = 0; t < nt; t++) {
        size_t s0 = t * chunk;
        size_t e0 = s0 + chunk;
        if (s0 > b->count) s0 = b->count;
        if (e0 > b->count) e0 = b->count;
        sl[t].c = c;
        sl[t].b = b;
        sl[t].start = s0;
        sl[t].end = e0;
        if (pthread_create(&th[t], NULL, cpu_slice, &sl[t]) != 0) die("pthread_create failed");
    }
    for (size_t t = 0; t < nt; t++) pthread_join(th[t], NULL);
}

#ifdef USE_CUDA

typedef struct {
    void *ptr;
    size_t cap;
} DevBuf;

static DevBuf g_in, g_out, g_off, g_len, g_outlen, g_mask;

static int dev_reserve(DevBuf *b, size_t bytes) {
    if (bytes == 0) bytes = 1;
    if (b->cap >= bytes) return 1;
    if (b->ptr) cudaFree(b->ptr);
    b->ptr = NULL;
    b->cap = 0;
    if (cudaMalloc(&b->ptr, bytes) != cudaSuccess) return 0;
    b->cap = bytes;
    return 1;
}

static void dev_release(DevBuf *b) {
    if (b->ptr) cudaFree(b->ptr);
    b->ptr = NULL;
    b->cap = 0;
}

static int dev_check(cudaError_t e, const char *what) {
    if (e == cudaSuccess) return 1;
    fprintf(stderr, "CUDA error during %s: %s\n", what, cudaGetErrorString(e));
    return 0;
}

__global__ static void reduce_kernel(const char *in, const size_t *off, const size_t *len,
                                     char *out, size_t *out_len, const unsigned char *mask,
                                     size_t count, unsigned flags, int preserve, RuleSet rules) {
    size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= count) return;

    size_t o = off[idx];
    size_t n = len[idx];

    if (!mask[idx]) {
        for (size_t k = 0; k < n; k++) out[o + k] = in[o + k];
        out_len[idx] = n;
        return;
    }
    out_len[idx] = transform_line(in + o, n, out + o, flags, preserve, &rules);
}

static int gpu_init(void) {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) {
        (void)cudaGetLastError();
        return 0;
    }
    return cudaSetDevice(0) == cudaSuccess ? 1 : 0;
}

static void gpu_shutdown(void) {
    dev_release(&g_in);
    dev_release(&g_out);
    dev_release(&g_off);
    dev_release(&g_len);
    dev_release(&g_outlen);
    dev_release(&g_mask);
}

static int gpu_transform(const char *in, const size_t *off, const size_t *len,
                         char *out, size_t *out_len, const unsigned char *mask,
                         size_t count, size_t total_bytes, unsigned flags,
                         int preserve, const RuleSet *rules) {
    if (count == 0) return 0;

    if (!dev_reserve(&g_in, total_bytes) || !dev_reserve(&g_out, total_bytes) ||
        !dev_reserve(&g_off, count * sizeof(size_t)) ||
        !dev_reserve(&g_len, count * sizeof(size_t)) ||
        !dev_reserve(&g_outlen, count * sizeof(size_t)) ||
        !dev_reserve(&g_mask, count)) {
        fprintf(stderr, "GPU allocation failed\n");
        return -1;
    }

    if (!dev_check(cudaMemcpy(g_in.ptr, in, total_bytes, cudaMemcpyHostToDevice), "input copy")) return -1;
    if (!dev_check(cudaMemcpy(g_off.ptr, off, count * sizeof(size_t), cudaMemcpyHostToDevice), "offset copy")) return -1;
    if (!dev_check(cudaMemcpy(g_len.ptr, len, count * sizeof(size_t), cudaMemcpyHostToDevice), "length copy")) return -1;
    if (!dev_check(cudaMemcpy(g_mask.ptr, mask, count, cudaMemcpyHostToDevice), "mask copy")) return -1;

    const int threads = 256;
    const unsigned blocks = (unsigned)((count + threads - 1) / threads);

    reduce_kernel<<<blocks, threads>>>(
        (const char *)g_in.ptr,
        (const size_t *)g_off.ptr,
        (const size_t *)g_len.ptr,
        (char *)g_out.ptr,
        (size_t *)g_outlen.ptr,
        (const unsigned char *)g_mask.ptr,
        count, flags, preserve, *rules);

    if (!dev_check(cudaGetLastError(), "kernel launch")) return -1;
    if (!dev_check(cudaDeviceSynchronize(), "kernel execution")) return -1;

    if (!dev_check(cudaMemcpy(out, g_out.ptr, total_bytes, cudaMemcpyDeviceToHost), "output copy")) return -1;
    if (!dev_check(cudaMemcpy(out_len, g_outlen.ptr, count * sizeof(size_t), cudaMemcpyDeviceToHost), "length readback")) return -1;

    return 0;
}

#endif

static void transform_batch(Ctx *c, Batch *b) {
    compute_mask(c, b);

    int use_gpu = c->gpu_ready &&
                  (c->backend == BACKEND_GPU ||
                   (c->backend == BACKEND_AUTO && b->bytes >= GPU_MIN_BYTES));

    if (use_gpu) {
#ifdef USE_CUDA
        if (gpu_transform(b->in, b->off, b->len, b->out, b->out_len, b->mask,
                          b->count, b->bytes, c->flags, c->preserve, &c->rules) == 0) {
            c->gpu_batches++;
            return;
        }
#endif
        fprintf(stderr, "Warning: GPU transform failed, falling back to CPU\n");
        c->gpu_ready = 0;
    }

    transform_cpu(c, b);
    c->cpu_batches++;
}

static void write_batch(Ctx *c, Batch *b, FILE *out) {
    for (size_t i = 0; i < b->count; i++) {
        fwrite(b->out + b->off[i], 1, b->out_len[i], out);
        fputc('\n', out);
        c->in_bytes  += b->len[i];
        c->out_bytes += b->out_len[i];
        c->lines++;
    }
}

static double elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_nsec - a->tv_nsec) / 1000000.0;
}

static void run_stream(Ctx *c, Batch *b, FILE *in, FILE *out) {
    struct timespec t0, t1;
    while (read_batch(in, b) > 0) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        transform_batch(c, b);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        c->tx_ms += elapsed_ms(&t0, &t1);
        write_batch(c, b, out);
    }
}

static int is_target_file(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (strcmp(dot, ".txt") == 0 || strcmp(dot, ".log") == 0 || strcmp(dot, ".md") == 0);
}

static void process_file(Ctx *c, Batch *b, const char *path) {
    char outpath[PATH_BUF];
    snprintf(outpath, sizeof outpath, "%s.reduced", path);

    FILE *in = fopen(path, "r");
    if (!in) { fprintf(stderr, "Warning: cannot open %s\n", path); return; }
    FILE *out = fopen(outpath, "w");
    if (!out) { fprintf(stderr, "Warning: cannot write %s\n", outpath); fclose(in); return; }

    run_stream(c, b, in, out);
    fclose(in);
    fclose(out);
    fprintf(stderr, "Processed: %s\n", path);
}

static void walk_dir(Ctx *c, Batch *b, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "Warning: cannot open directory %s\n", dir); return; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        char path[PATH_BUF];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            walk_dir(c, b, path);
        } else if (S_ISREG(st.st_mode) && is_target_file(e->d_name)) {
            process_file(c, b, path);
        }
    }
    closedir(d);
}

static void print_stats(const Ctx *c) {
    if (c->lines == 0) {
        fprintf(stderr, "No data processed\n");
        return;
    }

    const PricingModel *m = &MODELS[c->model_idx];
    double cpt = CHARS_PER_TOKEN[c->tokenizer];

    double in_tok  = (double)c->in_bytes / cpt;
    double out_tok = (double)c->out_bytes / cpt;
    double saved_tok = in_tok - out_tok;
    double saved_pct = in_tok > 0 ? 100.0 * saved_tok / in_tok : 0.0;

    double cost_before = in_tok / 1000000.0 * m->input_per_mtok;
    double cost_after  = out_tok / 1000000.0 * m->input_per_mtok;
    double saved_usd   = cost_before - cost_after;
    double saved_usd_pct = cost_before > 0 ? 100.0 * saved_usd / cost_before : 0.0;

    double secs = c->tx_ms / 1000.0;
    double mbps = secs > 0 ? ((double)c->in_bytes / 1048576.0) / secs : 0.0;

    fprintf(stderr, "\n========== COMPRESSION STATISTICS ==========\n");
    fprintf(stderr, "Model:               %s (%s)\n", m->name, m->family);
    fprintf(stderr, "Tokenizer:           %s\n", TOKENIZER_NAMES[c->tokenizer]);
    fprintf(stderr, "Lines processed:     %llu\n", c->lines);
    fprintf(stderr, "Input bytes:         %llu\n", c->in_bytes);
    fprintf(stderr, "Output bytes:        %llu\n", c->out_bytes);
    fprintf(stderr, "Est. input tokens:   %.0f\n", in_tok);
    fprintf(stderr, "Est. output tokens:  %.0f\n", out_tok);
    fprintf(stderr, "Tokens saved:        %.0f (%.2f%%)\n", saved_tok, saved_pct);
    fprintf(stderr, "Cost before:         $%.6f\n", cost_before);
    fprintf(stderr, "Cost after:          $%.6f\n", cost_after);
    fprintf(stderr, "Money saved:         $%.6f (%.2f%%)\n", saved_usd, saved_usd_pct);
    if (c->time_on) {
        fprintf(stderr, "Transform time:      %.2f ms\n", c->tx_ms);
        fprintf(stderr, "Throughput:          %.2f MB/s\n", mbps);
        fprintf(stderr, "Batches (GPU/CPU):   %lu / %lu\n", c->gpu_batches, c->cpu_batches);
    }
    fprintf(stderr, "============================================\n");
}

static void print_models(void) {
    fprintf(stderr, "\nSupported Pricing Models (input/output per 1M tokens):\n");
    fprintf(stderr, "======================================================\n");
    for (size_t i = 0; i < MODEL_COUNT; i++) {
        fprintf(stderr, "%2zu. %-20s $%.2f / $%.2f\n",
                i, MODELS[i].name, MODELS[i].input_per_mtok, MODELS[i].output_per_mtok);
    }
    fprintf(stderr, "\n");
}

static void print_help(const char *prog) {
    printf("TOKEN REDUCTION ENGINE v%s\n\n", VERSION);
    printf("Reduces text size before it is sent to an LLM. Reads stdin, writes stdout.\n\n");
    printf("USAGE:\n");
    printf("  %s [options] < input.txt\n", prog);
    printf("  %s [options] --batch <dir>     writes <file>.reduced beside each .txt/.log/.md\n\n", prog);
    printf("TRANSFORMATIONS (default with none given: --aggressive):\n");
    printf("  --ws           Collapse whitespace runs to one space (leading indentation is kept)\n");
    printf("  --filler       Remove filler words: really, very, quite, just, actually, basically\n");
    printf("  --abbr         Abbreviate common words (information -> info, thank you -> ty, ...)\n");
    printf("  --compress     Cap runs of 4+ decorative characters (- = _ * # ~) at three\n");
    printf("  --aggressive   --ws --filler --abbr\n\n");
    printf("FILTERING:\n");
    printf("  --preserve     Leave URLs, emails, and `backtick code` untouched\n");
    printf("  --match <re>   Transform only lines matching the extended regex (case-insensitive)\n\n");
    printf("EXECUTION:\n");
    printf("  --backend <b>  cpu | gpu | auto (default auto; GPU used for batches >= 4 MB)\n");
    printf("  --threads <n>  CPU worker threads, 1-%d (default %d)\n\n", MAX_THREADS, DEFAULT_THREADS);
    printf("COST ESTIMATE:\n");
    printf("  --tokenizer <n>  0=GPT-3 1=WordPiece 2=BPE 3=SentencePiece 4=Claude (default 0)\n");
    printf("  --model <n>      Pricing model index (default 0, see --show-models)\n");
    printf("  --show-models    List pricing models\n");
    printf("  --stats          Print statistics to stderr\n");
    printf("  --time           Statistics plus timing and throughput\n\n");
    printf("  -h, --help       Show this help\n\n");
    printf("EXAMPLES:\n");
    printf("  cat prompt.txt | %s --stats\n", prog);
    printf("  %s --preserve --match \"^LOG:\" --model 3 --stats < input.txt\n", prog);
    printf("  %s --batch ./logs --backend gpu --time\n", prog);
}

static int parse_long(const char *s, long lo, long hi, long *value) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < lo || v > hi) return -1;
    *value = v;
    return 0;
}

static const char *next_arg(int argc, char **argv, int *i) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "Fatal: %s requires an argument\n", argv[*i]);
        exit(1);
    }
    return argv[++(*i)];
}

int main(int argc, char **argv) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.threads = DEFAULT_THREADS;
    c.backend = BACKEND_AUTO;

    unsigned user_flags = 0;
    const char *batch_dir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        long v = 0;

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(a, "--show-models") == 0) {
            print_models();
            return 0;
        } else if (strcmp(a, "--ws") == 0) {
            user_flags |= MODE_WS;
        } else if (strcmp(a, "--filler") == 0) {
            user_flags |= MODE_FILLER;
        } else if (strcmp(a, "--compress") == 0) {
            user_flags |= MODE_COMPRESS;
        } else if (strcmp(a, "--abbr") == 0) {
            user_flags |= MODE_ABBR;
        } else if (strcmp(a, "--aggressive") == 0) {
            user_flags |= MODE_AGGRESSIVE;
        } else if (strcmp(a, "--preserve") == 0) {
            c.preserve = 1;
        } else if (strcmp(a, "--stats") == 0) {
            c.stats_on = 1;
        } else if (strcmp(a, "--time") == 0) {
            c.stats_on = 1;
            c.time_on = 1;
        } else if (strcmp(a, "--tokenizer") == 0) {
            if (parse_long(next_arg(argc, argv, &i), 0, TOKEN_COUNT - 1, &v) != 0)
                die("--tokenizer must be 0-4");
            c.tokenizer = (int)v;
        } else if (strcmp(a, "--model") == 0) {
            if (parse_long(next_arg(argc, argv, &i), 0, (long)MODEL_COUNT - 1, &v) != 0)
                die("--model index out of range (see --show-models)");
            c.model_idx = (int)v;
        } else if (strcmp(a, "--threads") == 0) {
            if (parse_long(next_arg(argc, argv, &i), 1, MAX_THREADS, &v) != 0)
                die("--threads must be between 1 and 64");
            c.threads = (int)v;
        } else if (strcmp(a, "--backend") == 0) {
            const char *b = next_arg(argc, argv, &i);
            if (strcmp(b, "cpu") == 0) c.backend = BACKEND_CPU;
            else if (strcmp(b, "gpu") == 0) c.backend = BACKEND_GPU;
            else if (strcmp(b, "auto") == 0) c.backend = BACKEND_AUTO;
            else die("--backend must be cpu, gpu, or auto");
        } else if (strcmp(a, "--match") == 0) {
            if (regcomp(&c.re, next_arg(argc, argv, &i),
                        REG_EXTENDED | REG_ICASE | REG_NOSUB) != 0)
                die("invalid regex");
            c.has_regex = 1;
        } else if (strcmp(a, "--batch") == 0) {
            batch_dir = next_arg(argc, argv, &i);
        } else {
            fprintf(stderr, "Unknown parameter: %s. Use --help for reference.\n", a);
            return 1;
        }
    }

    c.flags = user_flags ? user_flags : MODE_AGGRESSIVE;
    ruleset_init(&c.rules);

#ifdef USE_CUDA
    c.gpu_ready = gpu_init();
#endif
    if (c.backend == BACKEND_GPU && !c.gpu_ready) {
        die("--backend gpu requested, but no CUDA device is available or this build has no GPU support");
    }

    Batch b;
    if (batch_init(&b) != 0) die("out of memory");

    if (batch_dir) {
        walk_dir(&c, &b, batch_dir);
    } else {
        run_stream(&c, &b, stdin, stdout);
    }

    fflush(stdout);
    if (c.stats_on) print_stats(&c);

    batch_free(&b);
    if (c.has_regex) regfree(&c.re);
#ifdef USE_CUDA
    gpu_shutdown();
#endif
    return 0;
}
