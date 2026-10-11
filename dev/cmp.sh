#!/bin/bash
# usage: dev/cmp.sh "<cmdA>" "<cmdB>" pairs movetime concurrency tag [seed0]
# Each cmd is an engine command line (env vars allowed via 'env VAR=x ./engine').
cd /home/user/cascade-kit
A="$1"; B="$2"; PAIRS="$3"; MT="$4"; CONC="$5"; TAG="$6"; SEED="${7:-1}"
mkdir -p results
cat > results/$TAG.json <<JSON
{ "engines": [
  { "name": "A", "cmd": "$A", "go": "${GO:-movetime $MT}" },
  { "name": "B", "cmd": "$B", "go": "${GO:-movetime $MT}" } ],
  "pairs": $PAIRS, "concurrency": $CONC, "seed": $SEED, "out": "results/$TAG" }
JSON
rm -rf results/$TAG
node referee/tournament.js results/$TAG.json 2>&1 | grep -E "^A |^B |games;|endings" 
