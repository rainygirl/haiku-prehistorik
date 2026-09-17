#!/bin/sh
# Retranslate and rebuild the headless test binary.
cd "$(dirname "$0")/.." && python3 tools/translate.py original/historik.exe src/gen | grep -v "^ " && make -f tools/host/Makefile -j10 2>&1 | grep -E "error" | head
