#!/usr/bin/env bash

folder=${1:-sample files}
count=0

for file in "$folder"/* "$folder"/.[!.]* "$folder"/..?*; do
    if [ ! -L "$file" ] && [ -f $file ]; then
        ((count += 1))
    fi
done

printf '%d\n' "$count"
