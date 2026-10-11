#!/usr/bin/env python3
# Builds submission/engine.cpp from dev/engine_dev.cpp: drops // DEV-BEGIN .. // DEV-END blocks
# and enables lines prefixed with //LEAN:
import sys, re
src = open('dev/engine_dev.cpp').read().split('\n')
out = []
skip = False
for line in src:
    t = line.strip()
    if t == '// DEV-BEGIN': skip = True; continue
    if t == '// DEV-END': skip = False; continue
    if skip: continue
    if line.startswith('//LEAN: '): line = line[len('//LEAN: '):]
    out.append(line)
open('submission/engine.cpp', 'w').write('\n'.join(out))
