#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* Configurable Constants */
#define BUFFER_SIZE 65536

/* Bitmask Flags for Advanced Multi-Option Routing */
typedef enum {
    MODE_WS         = 1 << 0,
    MODE_FILLER     = 1 << 1,
    MODE_COMPRESS   = 1 << 2,
    MODE_ABBR       = 1 << 3,
    MODE_AGGRESSIVE = MODE_WS | MODE_FILLER | MODE_ABBR
} ProcessingMode;

/* High-Performance Lookup Tables */
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
    {"information", "info", 11}, {"government", "gov", 10}, {"please", "pls", 6},
    {"thank you", "ty", 9},       {"because", "bc", 7},      {"through", "thru", 7},
    {"example", "eg", 7},         {"important", "imp", 9},   {"different", "diff", 9},
    {"community", "comm", 9}
};
#define ABBREV_COUNT (sizeof(ABBREVS) / sizeof(ABBREVS[0]))

/* Helper: Validates if a substring matches with true word boundaries */
static inline bool is_valid_bound(const char *base, const char *start, size_t match_len) {
    bool left_ok = (start == base) || !isalnum((unsigned char)*(start - 1));
    bool right_ok = !isalnum((unsigned char)*(start + match_len));
    return left_ok && right_ok;
}

/* Linear-Time Single Pass Transformer */
void process_stream(const char *src, char *dst, int flags) {
    size_t i = 0, j = 0;
    bool in_whitespace = false;

    while (src[i] != '\0') {
        /* 1. Dynamic Compression: Collapse repeating characters */
        if ((flags & MODE_COMPRESS) && src[i + 1] == src[i] && !isspace((unsigned char)src[i])) {
            i++;
            continue;
        }

        /* 2. Whitespace Management: Standardizes all spaces/tabs into a single ' ' */
        if (flags & MODE_WS) {
            if (isspace((unsigned char)src[i])) {
                if (!in_whitespace) {
                    dst[j++] = ' ';
                    in_whitespace = true;
                }
                i++;
                continue;
            }
            in_whitespace = false;
        }

        /* 3. Token Boundary Engine: Intercepts full words for structural dictionary translation */
        if ((flags & (MODE_FILLER | MODE_ABBR)) && ((i == 0) || !isalnum((unsigned char)src[i - 1]))) {
            bool matched = false;

            /* Check Abbreviation Dictionary first */
            if (flags & MODE_ABBR) {
                for (size_t k = 0; k < ABBREV_COUNT; k++) {
                    if (strncasecmp(&src[i], ABBREVS[k].full, ABBREVS[k].len) == 0 &&
                        is_valid_bound(src, &src[i], ABBREVS[k].len)) {
                        memcpy(&dst[j], ABBREVS[k].abbr, strlen(ABBREVS[k].abbr));
                        j += strlen(ABBREVS[k].abbr);
                        i += ABBREVS[k].len;
                        matched = true;
                        break;
                    }
                }
            }

            /* Check Filler Dictionary second */
            if (!matched && (flags & MODE_FILLER)) {
                for (size_t k = 0; k < FILLER_COUNT; k++) {
                    size_t filler_len = strlen(FILLERS[k]);
                    if (strncasecmp(&src[i], FILLERS[k], filler_len) == 0 &&
                        is_valid_bound(src, &src[i], filler_len)) {
                        i += filler_len;
                        /* Advance past immediately trailing space to prevent orphan padding */
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

        /* Standard copy step if no parsing rules intercepted the byte */
        dst[j++] = src[i++];
    }
    dst[j] = '\0';
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [options] < input_text\n\n", prog_name);
    printf("Options:\n");
    printf("  --ws          Standardize and shrink whitespace sequences\n");
    printf("  --filler      Strip standalone conversational filler tokens\n");
    printf("  --compress    Collapse identical repeating alphanumeric characters\n");
    printf("  --abbr        Apply dictionary-driven text abbreviations\n");
    printf("  --aggressive  Activate --ws, --filler, and --abbr simultaneously (default)\n");
    printf("  --help        Display this technical manifest\n");
}

int main(int argc, char *argv[]) {
    int active_flags = 0;

    /* Parse Arguments natively to allow compound options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--ws") == 0) active_flags |= MODE_WS;
        else if (strcmp(argv[i], "--filler") == 0) active_flags |= MODE_FILLER;
        else if (strcmp(argv[i], "--compress") == 0) active_flags |= MODE_COMPRESS;
        else if (strcmp(argv[i], "--abbr") == 0) active_flags |= MODE_ABBR;
        else if (strcmp(argv[i], "--aggressive") == 0) active_flags |= MODE_AGGRESSIVE;
        else {
            fprintf(stderr, "Unknown parameter identified: %s\n", argv[i]);
            return 1;
        }
    }

    /* Fallback to systemic defaults if no distinct flags were picked */
    if (active_flags == 0) {
        active_flags = MODE_AGGRESSIVE;
    }

    /* Stream-based chunking buffers allocated to heap instead of dangerous stack structures */
    char *in_buf = malloc(BUFFER_SIZE);
    char *out_buf = malloc(BUFFER_SIZE);
    if (!in_buf || !out_buf) {
        fprintf(stderr, "Fatal error: Core heap allocation failure.\n");
        free(in_buf); free(out_buf);
        return 1;
    }

    /* Process incoming streams line-by-line indefinitely */
    while (fgets(in_buf, BUFFER_SIZE, stdin) != NULL) {
        process_stream(in_buf, out_buf, active_flags);
        printf("%s", out_buf);
    }

    free(in_buf);
    free(out_buf);
    return 0;
}
