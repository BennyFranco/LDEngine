#!/bin/bash
clang ldengine.c -I/usr/local/include -L/usr/local/lib -F./frameworks -framework SDL2 -o build/LDEngine -O0 -g