#!/bin/sh
# Builds the two Vulkan helpers of tools/shaderstats into tools/shaderstats/bin/ (needs the Vulkan headers + loader and gcc).
set -e
cd "$(dirname "$0")"
mkdir -p bin
gcc -O1 -o bin/vkstats vkstats.c -lvulkan
gcc -O1 -o bin/vkrun vkrun.c -lvulkan
echo "built $(pwd)/bin/vkstats $(pwd)/bin/vkrun"
