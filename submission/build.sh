#!/bin/sh
# Builds the engine.  Tries progressively plainer compiler settings so that an
# unexpected toolchain still produces a binary, keeps any binary that shipped with
# the submission if nothing compiles, and always exits 0 so that run.sh can fall
# back to the bundled JavaScript engine as a last resort.
CXX=${CXX:-g++}
command -v "$CXX" >/dev/null 2>&1 || CXX=c++
for FLAGS in "-O3 -march=x86-64-v2 -funroll-loops" "-O3" "-O2" "-O1"; do
  if $CXX $FLAGS -std=c++17 -pthread -o engine.new engine.cpp 2>>build.log; then
    mv -f engine.new engine
    chmod +x engine
    echo "built with: $CXX $FLAGS"
    exit 0
  fi
done
rm -f engine.new
echo "WARNING: could not compile engine.cpp" >&2
[ -f build.log ] && tail -40 build.log >&2
exit 0
