#!/bin/sh
# Build the host simulation (needs vita-headers + libvita2d include dirs).
#   VITA_HEADERS=/path/to/vita-headers/include VITA2D_INC=/path/to/libvita2d/include sh tests/host_sim/build.sh
set -e
cd "$(dirname "$0")/../.."
: "${VITA_HEADERS:=$VITASDK/arm-vita-eabi/include}"
: "${VITA2D_INC:=$VITASDK/arm-vita-eabi/include}"
cc -O1 -g -std=gnu99 -Wall -Wno-unused-parameter -pthread \
   -Iinclude -isystem "$VITA_HEADERS" -I"$VITA2D_INC" \
   tests/host_sim/sim_driver.c tests/host_sim/sim_stubs.c \
   src/game_logic.c src/ai.c src/audio.c src/render.c src/adhoc_net.c \
   -lm -o tests/host_sim/bs_sim
echo built tests/host_sim/bs_sim
