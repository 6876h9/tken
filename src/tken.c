#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define MAX_INPUT 8192
#define MAX_OUTPUT 4096

/* Token reduction algorithms */

/* Remove excessive whitespace */
void reduce_whitespace(const char *input, char *output) {
    int i = 0, j = 0;
    int last_space = 0;
    
    while (input[i] && j < MAX_OUTPUT - 1) {
        if (isspace(input[i])) {
            if (!last_space) {
                output[j++] = ' ';
                last_space = 1;
            }
            i++;
        } else {
            output[j++] = input[i];
            last_space = 0;
            i++;
        }
    }
    output[j] = '\0';
}

/* Remove common filler words only when standalone */
void remove_filler(const char *input, char *output) {
    const char *fillers[] = {
        " really ", " very ", " quite ", " just ", " actually ", " basically ",
        " the ", " a ", " an ", NULL
    };
    
    strcpy(output, input);
    
    for (int i = 0; fillers[i] != NULL; i++) {
        char *pos;
        while ((pos = strstr(output, fillers[i])) != NULL) {
            /* Remove the word but keep one space */
            memmove(pos + 1, pos + strlen(fillers[i]), strlen(pos + strlen(fillers[i])) + 1);
        }
    }
}

/* Compress repeated characters */
void compress_chars(const char *input, char *output) {
    int i = 0, j = 0;
    
    while (input[i] && j < MAX_OUTPUT - 1) {
        output[j++] = input[i];
        char current = input[i];
        
        while (input[i + 1] == current && input[i + 1] != '\0') {
            i++;
        }
        i++;
    }
    output[j] = '\0';
}

/* Abbreviate common words */
void abbreviate(const char *input, char *output) {
    struct {
        const char *full;
        const char *abbr;
    } abbrevs[] = {
        {"information", "info"},
        {"government", "gov"},
        {"please", "pls"},
        {"thank you", "ty"},
        {"because", "bc"},
        {"through", "thru"},
        {"example", "eg"},
        {"important", "imp"},
        {"different", "diff"},
        {"community", "comm"},
        {NULL, NULL}
    };
    
    strcpy(output, input);
    
    for (int i = 0; abbrevs[i].full != NULL; i++) {
        char *pos;
        while ((pos = strstr(output, abbrevs[i].full)) != NULL) {
            char temp[MAX_OUTPUT];
            int offset = pos - output;
            strcpy(temp, pos + strlen(abbrevs[i].full));
            strcpy(pos, abbrevs[i].abbr);
            strcat(pos, temp);
        }
    }
}

/* Combined aggressive reduction - skip char compression */
void aggressive_reduce(const char *input, char *output) {
    reduce_whitespace(input, output);
    remove_filler(output, output);
}

void print_usage() {
    printf("Usage: tken [options] < input_text\n\n");
    printf("Options:\n");
    printf("  --ws      Remove excessive whitespace\n");
    printf("  --filler  Remove common filler words\n");
    printf("  --compress Compress repeated characters\n");
    printf("  --abbr    Abbreviate common words\n");
    printf("  --aggressive All techniques (default)\n");
    printf("  --help    Show this help message\n\n");
    printf("Example: echo 'This is very very important information' | tken --aggressive\n");
}

int main(int argc, char *argv[]) {
    char input[MAX_INPUT] = {0};
    char output[MAX_OUTPUT] = {0};
    int mode = 0; /* 0 = aggressive, 1 = ws, 2 = filler, 3 = compress, 4 = abbr */
    
    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[1], "--ws") == 0) {
            mode = 1;
        } else if (strcmp(argv[1], "--filler") == 0) {
            mode = 2;
        } else if (strcmp(argv[1], "--compress") == 0) {
            mode = 3;
        } else if (strcmp(argv[1], "--abbr") == 0) {
            mode = 4;
        } else if (strcmp(argv[1], "--aggressive") == 0) {
            mode = 0;
        }
    }
    
    /* Read from stdin */
    if (fgets(input, MAX_INPUT, stdin) == NULL) {
        fprintf(stderr, "Error: Could not read input\n");
        return 1;
    }
    
    /* Remove trailing newline */
    size_t len = strlen(input);
    if (len > 0 && input[len - 1] == '\n') {
        input[len - 1] = '\0';
    }
    
    /* Apply selected mode */
    switch (mode) {
        case 1:
            reduce_whitespace(input, output);
            break;
        case 2:
            remove_filler(input, output);
            break;
        case 3:
            compress_chars(input, output);
            break;
        case 4:
            abbreviate(input, output);
            break;
        case 0:
        default:
            aggressive_reduce(input, output);
            break;
    }
    
    printf("%s\n", output);
    return 0;
}
