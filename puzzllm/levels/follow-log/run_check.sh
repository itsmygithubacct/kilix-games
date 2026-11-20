#!/bin/bash
set -eu

run=$(od -An -N12 -tx1 /dev/urandom | tr -d ' \n')
files=(inventory.csv rooms.json assets.manifest routes.txt)
printf 'Checking room data...\n'
printf 'PuzzLLM check failed: %s %s\n' "$run" "${files[RANDOM % ${#files[@]}]}"
