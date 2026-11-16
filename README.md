# tken - Token Reduction Engine

Version 4.0

tken reduces the size of text before it is sent to a large language model. It removes filler words, abbreviates common words, collapses whitespace, and estimates the token count and API cost before and after reduction. It optionally runs on an NVIDIA GPU through CUDA.

tken changes the text it processes. Review the output before using it where exact wording matters.

## Features

- Whitespace collapsing that keeps line breaks and leading indentation
- Removal of filler words: really, very, quite, just, actually, basically
- Abbreviation of common words: information to info, government to gov, thank you to ty, and others
- Capping of decorative character runs such as `-----` (optional)
- Preservation of URLs, email addresses, and backtick code spans (optional)
- Regex-based line selection with `--match`
- Batch processing of `.txt`, `.log`, and `.md` files in a directory tree
- Token estimates and cost calculation for 11 pricing models across five tokenizer families
- Optional CUDA backend, selected automatically for large batches

## Requirements

- A C11 compiler (GCC or Clang) and POSIX threads
- For GPU support: an NVIDIA GPU, the CUDA Toolkit (nvcc), and a compatible driver

## Building

CPU build:

```
make
```

CUDA build. Set `ARCH` to match your GPU:

| GPU generation | ARCH |
|----------------|------|
| Turing (RTX 20 series, GTX 16 series) | sm_75 |
| Ampere (RTX 30 series) | sm_86 |
| Ada Lovelace (RTX 40 series) | sm_89 |

```
make gpu ARCH=sm_86
```

Both targets can be built side by side. The CUDA build produces `tken_gpu`.

## Testing

Run the CPU test, which writes a sample run to `cpu.out`:

```
make test
```

Run the CPU and GPU comparison. Both outputs must match byte for byte:

```
make test-gpu ARCH=sm_86
```

If the outputs differ, the host and device paths disagree. Do not rely on GPU output until this check passes.

Remove build products and test files:

```
make clean
```

## Usage

tken reads stdin and writes stdout. Statistics are written to stderr.

```
./tken [options] < input.txt
./tken [options] --batch <directory>
```

Examples:

```
cat prompt.txt | ./tken --stats
./tken --preserve --match "^LOG:" --model 3 --stats < input.txt
./tken --batch ./logs --backend gpu --time
```

## Options

### Transformations

If no transformation flag is given, `--aggressive` is used.

| Flag | Effect |
|------|--------|
| `--ws` | Collapse runs of whitespace to one space. Line breaks and leading indentation are kept. |
| `--filler` | Remove the words really, very, quite, just, actually, and basically. |
| `--abbr` | Replace common words with abbreviations (see the table below). |
| `--compress` | Reduce runs of four or more of `- = _ * # ~` to three characters. Other text is not changed. |
| `--aggressive` | Equivalent to `--ws --filler --abbr`. |

Abbreviations:

| Full | Abbreviation | Full | Abbreviation |
|------|--------------|------|--------------|
| information | info | through | thru |
| government | gov | example | eg |
| please | pls | important | imp |
| thank you | ty | different | diff |
| because | bc | community | comm |

Matching is case-insensitive and applies only to whole words.

### Filtering

| Flag | Effect |
|------|--------|
| `--preserve` | Leave URLs, `www.` addresses, email addresses, and text inside backtick code spans unchanged. |
| `--match <regex>` | Transform only lines that match the extended regular expression. Matching is case-insensitive. Other lines pass through unchanged. |

### Execution

| Flag | Effect |
|------|--------|
| `--backend cpu` | Use the CPU only. |
| `--backend gpu` | Use the GPU for every batch. The program exits if no CUDA device is available. |
| `--backend auto` | Default. Use the GPU for batches of 4 MB or more and the CPU otherwise. |
| `--threads <n>` | Number of CPU worker threads, from 1 to 64. Default 4. |
| `--batch <dir>` | Process every `.txt`, `.log`, and `.md` file under `<dir>` recursively. Each output is written beside its input as `<name>.reduced`. |

### Cost estimate

| Flag | Effect |
|------|--------|
| `--tokenizer <n>` | Tokenizer used for the token estimate. See the table below. Default 0. |
| `--model <n>` | Pricing model used for the cost estimate. Default 0. |
| `--show-models` | List the pricing models with their indexes. |
| `--stats` | Print statistics to stderr. |
| `--time` | Print statistics plus transform time, throughput, and the number of GPU and CPU batches. |
| `-h`, `--help` | Show help. |

Tokenizers:

| Index | Name | Characters per token used for estimate |
|-------|------|----------------------------------------|
| 0 | GPT-3 | 4.0 |
| 1 | WordPiece | 5.0 |
| 2 | BPE | 4.5 |
| 3 | SentencePiece | 5.0 |
| 4 | Claude | 3.7 |

Pricing models:

| Index | Model | Input per 1M tokens | Output per 1M tokens |
|-------|-------|---------------------|----------------------|
| 0 | GPT-4o | $2.50 | $10.00 |
| 1 | GPT-4o mini | $0.15 | $0.60 |
| 2 | GPT-3.5 Turbo | $0.50 | $1.50 |
| 3 | Claude Opus 4.8 | $5.00 | $25.00 |
| 4 | Claude Sonnet 4.6 | $3.00 | $15.00 |
| 5 | Claude Haiku 4.5 | $1.00 | $5.00 |
| 6 | Claude Fable 5 | $10.00 | $50.00 |
| 7 | Gemini 3.1 Pro | $2.00 | $12.00 |
| 8 | Gemini 2.5 Pro | $1.25 | $5.00 |
| 9 | Grok 4.1 | $0.20 | $0.50 |
| 10 | Llama 3.1 | $0.50 | $1.50 |

Prices must be verified against each vendor's current pricing page before the cost output is used for decisions. Use `--show-models` to list the indexes.

## How cost is calculated

Token counts are estimated from byte counts divided by the characters-per-token value of the selected tokenizer. They are not exact tokenizer output. Prompt tokens are billed at the model's input rate, so the reduction is priced at the input rate only:

```
cost_before = input_tokens / 1,000,000 * input_price
cost_after  = output_tokens / 1,000,000 * input_price
```

## Statistics output

```
========== COMPRESSION STATISTICS ==========
Model:               Claude Opus 4.8 (anthropic)
Tokenizer:           Claude (3.7 chars/token)
Lines processed:     ...
Input bytes:         ...
Output bytes:        ...
Est. input tokens:   ...
Est. output tokens:  ...
Tokens saved:        ... (...%)
Cost before:         $...
Cost after:          $...
Money saved:         $... (...%)
Transform time:      ... ms        (with --time)
Throughput:          ... MB/s      (with --time)
Batches (GPU/CPU):   ... / ...     (with --time)
============================================
```

## Performance

- The CPU backend splits each batch across worker threads.
- The GPU backend runs one thread per line. Host-to-device copies have a fixed cost, so the GPU is only faster for large batches. Use `--time` to compare the two backends on your own data before choosing one.
- Batches hold up to 8192 lines or 8 MB of input, whichever comes first. Lines of any length are accepted.

## Limitations

- Transformations are lossy. The original text cannot be recovered from the output.
- Token counts are estimates based on character counts, not real tokenizer output.
- Abbreviations and filler words are fixed. There is no custom dictionary yet.
- `--match` decides per line. It does not transform only part of a line.
- The GPU backend has not been benchmarked on all hardware. Verify output with `make test-gpu` before use.

## Version history

- **4.0**: Single source file. Output keeps line structure. `--match` transforms whole lines. `--preserve` checks whole tokens. `--compress` affects only decorative runs. Articles are no longer removed. Cost is priced at the input rate. Batch mode and optional CUDA backend added.
- **3.0**: Renamed to tken. Multiple tokenizers and pricing models. Earlier versions did not keep line breaks and did not implement batch mode.
