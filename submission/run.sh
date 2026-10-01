#!/bin/sh
if [ -x ./engine ]; then exec ./engine; fi
exec node fallback.js
