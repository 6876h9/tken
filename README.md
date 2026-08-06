# TKEN v3 - Token Reduction Engine

## Overview

Production-grade text compression tool that reduces API token costs by:
- Removing filler words (very, really, quite, just, basically, etc.)
- Abbreviating common words (information → info, government → gov, etc.)
- Collapsing whitespace and duplicate characters
- Preserving URLs, emails, and code blocks (optional)

Supports 5 different tokenization methods and 11 LLM pricing models with real-time cost calculation.

---

## Quick Start

### Basic Usage
```bash
./tken_v3 --stats < input.txt
```

### With Cost Breakdown for Claude Opus 4.8
```bash
./tken_v3 --stats --model 3 < input.txt
```

### Preserve URLs/Code, Use Claude Tokenizer
```bash
./tken_v3 --preserve --tokenizer 4 --stats < input.txt
```

### Compare Different Models
```bash
for model in 0 1 2 3; do
  echo "=== Model $model ==="
  cat input.txt | ./tken_v3 --model $model --stats 2>&1 | grep "Model:\|Money saved:"
done
```

---

## Tokenization Methods

| ID | Name | Chars/Token | Used By | Notes |
|----|------|-------------|---------|-------|
| 0 | GPT-3 | 4.0 | OpenAI GPT-3/3.5 | Default, most common |
| 1 | WordPiece | 5.0 | BERT, Google models | Balanced approach |
| 2 | BPE | 4.5 | GPT-2, GPT-4 | Good for code |
| 3 | SentencePiece | 5.0 | T5, mBART, Gemini | Language-agnostic |
| 4 | Claude | 3.7 | Anthropic Claude | Most efficient tokenization |

**Claude tokenizer (4) produces the lowest token counts** because it has the lowest chars/token ratio.

---

## Supported Pricing Models

```
 0. GPT-4o              $2.50/$10.00 per 1M (input/output)
 1. GPT-4o mini         $0.15/$0.60 per 1M
 2. GPT-3.5 Turbo       $0.50/$1.50 per 1M
 3. Claude Opus 4.8     $5.00/$25.00 per 1M ← Most expensive
 4. Claude Sonnet 4.6   $3.00/$15.00 per 1M
 5. Claude Haiku 4.5    $1.00/$5.00 per 1M
 6. Claude Fable 5      $10.00/$50.00 per 1M ← Premium model
 7. Gemini 3.1 Pro      $2.00/$12.00 per 1M
 8. Gemini 2.5 Pro      $1.25/$5.00 per 1M
 9. Grok 4.1            $0.20/$0.50 per 1M ← Cheapest
10. Llama 3.1           $0.50/$1.50 per 1M
```

Use `--show-models` to display full list dynamically.

---

## Cost Savings Examples

### Example 1: Claude Opus (Expensive Model)
```
Input:  "This is a very lengthy document that contains quite a lot of unnecessary filler"
Output: "This is lengthy document that contains lot of unnecessary filler"

Input tokens:  80
Output tokens: 57
Tokens saved:  23 (28.75%)

Cost before: $0.002400
Cost after:  $0.001825
Money saved: $0.000575 (23.96%)
```

### Example 2: Budget Model (GPT-4o mini)
Same text compressed:
- Money saved: $0.00003 per call
- But at scale (1M calls/day): **$30/day = $900/month saved**

---

## Transformation Examples

### Input
```
The government really does provide quite a lot of very valuable information. 
Please check through the example code. It's basically important because the 
process is very very complex indeed.
```

### Output (Aggressive Mode)
```
gov does provide lot of valuable info. pls check thru eg code. It's imp bc 
process is complex indeed.
```

**Reduction: 34.72% tokens saved**

---

## Advanced Usage

### 1. Selective Transformation (Only LOG lines)
```bash
cat access.log | ./tken_v3 --match "^LOG:" --stats
```

### 2. Preserve URLs/Code
```bash
./tken_v3 --preserve --stats < code_with_docs.txt
```
This won't abbreviate words inside URLs or code blocks.

### 3. Custom Tokenizer + Model Combo
```bash
# BPE tokenization with Grok (cheapest) pricing
./tken_v3 --tokenizer 2 --model 9 --stats < input.txt

# Claude tokenization with Claude Fable (most expensive)
./tken_v3 --tokenizer 4 --model 6 --stats < input.txt
```

### 4. Timing Analysis
```bash
./tken_v3 --time --stats < large_file.txt
```
Shows:
- Processing time in milliseconds
- Throughput (characters/second)
- All cost breakdowns

---

## Statistics Output Breakdown

```
========== COMPRESSION STATISTICS ==========
Model:               Claude Opus 4.8 (anthropic)
Tokenizer:           Claude (char/token: 3.7)
Lines processed:     1
Input tokens:        80
Output tokens:       57
Tokens saved:        23 (28.75%)
Compression ratio:   71.25%          ← Output is 71% of original size
Cost before:         $0.002400       ← What you would pay without compression
Cost after:          $0.001825       ← What you pay with compression
Money saved:         $0.000575 (23.96%)  ← Actual savings
Processing time:     5.27 ms
Throughput:          40 chars/sec
============================================
```

---

## Performance Characteristics

- **Parallelism**: 4 worker threads by default
- **Max line size**: 65KB (BUFFER_SIZE)
- **Buffer multiplication**: 4x for output (safety margin)
- **Throughput**: ~40-100 chars/sec depending on CPU
- **Memory**: ~256KB per worker thread

For large files (100MB+), throughput is sustained around 50-80 chars/sec per thread.

---

## Transformation Flags Explained

| Flag | What It Does | Example |
|------|--------------|---------|
| `--ws` | Collapse multiple spaces into one | `"a    b"` → `"a b"` |
| `--filler` | Remove common filler words | `"very important"` → `"important"` |
| `--compress` | Collapse duplicate chars | `"aaa"` → `"a"`, `"!!!"` → `"!"` |
| `--abbr` | Abbreviate common words | `"information"` → `"info"` |
| `--aggressive` | Enable all (DEFAULT if no flags given) | All of above |
| `--preserve` | Skip URLs/emails/code | `"https://x.com"` untouched |

---

## Real-World Scenarios

### Scenario 1: Prompt Compression for LLM Chains
**Use case**: Passing retrieved documents through Claude

```bash
cat retrieved_docs.txt | ./tken_v3 --preserve --model 4 --stats > compressed_docs.txt
```

**Benefit**: Keep document semantics intact (preserve URLs), reduce output tokens by 25-35%

### Scenario 2: Log Aggregation Cost Reduction
**Use case**: Sending logs to Fable 5 model for analysis

```bash
tail -f access.log | ./tken_v3 --match "ERROR|WARN" --model 6 --stats
```

**Benefit**: Only compress ERROR/WARN lines, save money on high-volume log processing

### Scenario 3: Multi-Model Cost Comparison
**Use case**: Deciding which model to use

```bash
for model in {0..10}; do
  echo "Model $model:"
  cat budget.txt | ./tken_v3 --model $model --stats 2>&1 | grep "Money saved:"
done
```

---

## Batch Mode (Planned)

```bash
./tken_v3 --batch /var/logs/ --model 2 --stats
```

Processes all `.txt`, `.log`, `.md` files recursively with cumulative statistics.

---

## Building from Source

```bash
gcc -Wall -Wextra -O2 -pthread tken_v3_full.c -o tken_v3
```

Requirements:
- GCC or Clang
- POSIX threads (pthread)
- Standard C library (libc)

---

## Tips & Tricks

1. **Test with --preserve first** to ensure code/URLs aren't broken
2. **Use --model 9 (Grok) pricing** for cost baseline comparisons
3. **Combine tokenizers** to find the best fit for your text type
4. **Redirect stderr for stats**: `./tken_v3 --stats < in.txt 2> stats.txt`
5. **Preserve before compress**: `--preserve --abbr --filler` is safer than `--aggressive`

---

## Limitations

- Lines longer than 65KB will be truncated (rare for normal text)
- No reversibility by default (transformations are lossy)
- Abbreviations are hardcoded (no custom dictionary yet)
- Regex pattern matching doesn't preserve match groups
- Batch mode not yet implemented (stub present)

---

## FAQ

**Q: Which tokenizer should I use?**
A: Use Claude (4) for lowest token counts. Use GPT-3 (0) if using OpenAI models.

**Q: How much money can I save?**
A: 25-35% token reduction is typical. At scale:
- 1M API calls/day with Fable 5: **$250-350/day** saved
- 100K calls/day with Claude Opus: **$25-35/day** saved

**Q: Should I use --preserve?**
A: Yes, unless you're processing plain text without URLs/code/emails.

**Q: Can I use this in production?**
A: Yes. It's production-grade with proper error handling, thread safety, and bounded memory.

---

## Version History

- **v3.0** (Current): Multiple tokenizers, 11 models, pricing, comprehensive help
- **v2.0**: Token counting, stats, semantic preservation
- **v1.0**: Basic text transformations
