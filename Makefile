CC         ?= gcc
NVCC       ?= nvcc
ARCH       ?= sm_75
CFLAGS     ?= -O3 -std=c11 -Wall -Wextra -pedantic
NVCCFLAGS  ?= -O3 -arch=$(ARCH) -DUSE_CUDA -x cu -Xcompiler -pthread
LDLIBS     = -lpthread

SRC        = tken.c
CPU_BIN    = tken
GPU_BIN    = tken_gpu

.PHONY: all cpu gpu test test-cpu test-gpu clean

all: cpu

cpu: $(CPU_BIN)

gpu: $(GPU_BIN)

$(CPU_BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

$(GPU_BIN): $(SRC)
	$(NVCC) $(NVCCFLAGS) -o $@ $(SRC) $(LDLIBS)

sample.txt:
	@printf 'The government really does provide very valuable information.\n' > $@
	@printf '\n' >> $@
	@printf 'Please see https://example.com   for the example code.\n' >> $@
	@printf 'Contact admin@example.com if it is basically important.\n' >> $@
	@printf '==========================================\n' >> $@
	@printf '  Indented   line    with   spaces\n' >> $@
	@printf 'Code: `very   raw   code` stays as written.\n' >> $@

test-cpu: $(CPU_BIN) sample.txt
	./$(CPU_BIN) --preserve --backend cpu --stats < sample.txt > cpu.out
	@echo "CPU output written to cpu.out"

test-gpu: $(CPU_BIN) $(GPU_BIN) sample.txt
	./$(CPU_BIN) --backend cpu < sample.txt > cpu.out
	./$(GPU_BIN) --backend gpu < sample.txt > gpu.out
	cmp cpu.out gpu.out && echo "PASS: CPU and GPU outputs match"

test: test-cpu

clean:
	rm -f $(CPU_BIN) $(GPU_BIN) *.o cpu.out gpu.out sample.txt
