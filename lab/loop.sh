#!/bin/bash
# Self-play improvement loop. Usage: loop.sh START_ITER END_ITER
D=/tmp/claude-0/t
cd /home/user/cascade-kit/lab
for i in $(seq $1 $2); do
  prev=$((i-1))
  echo "=== iter $i: gen with n$prev" 
  ./gen -g ${GAMES:-6000} -nodes ${NODES:-10000} -s $((i*1000003)) -net $D/n$prev.nn -o $D/d$i.bin 2>&1 | tail -1
  files=$(ls $D/d*.bin | sort -V | tail -${WINDOW:-4})
  echo "train on $files"
  python3 train.py $files --epochs ${EPOCHS:-10} --lam ${LAM:-0.25} --out $D/n$i.pt 2>&1 | tail -2
  python3 export.py $D/n$i.pt $D/n$i.nn
  ./lab -n 100 -c ms=20 -A net=$D/n$i.nn -B net=$D/n$prev.nn 2>/dev/null
done
