# Installation Guide

## Quick Start

### Linux/macOS
```bash
git clone https://github.com/yourusername/tken.git
cd tken
make
sudo make install
```

### Windows (MSYS2/MinGW)
```bash
git clone https://github.com/yourusername/tken.git
cd tken
gcc -O2 -o tken.exe src/tken.c
```

Then add to PATH:
1. Copy `tken.exe` to `C:\Program Files\tken\`
2. Add `C:\Program Files\tken` to system PATH environment variable
3. Restart Command Prompt

## Verify Installation

```bash
tken --help
```

## Uninstall

```bash
sudo make uninstall
```
