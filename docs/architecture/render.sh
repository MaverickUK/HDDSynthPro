#!/bin/bash
# Checks workspace.dsl and redraws the diagrams: Mermaid exports (mermaid/)
# and SVGs from them (images/), with Docker (Structurizr and the Mermaid CLI).
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD:/w" structurizr/structurizr validate -w /w/workspace.dsl
rm -rf mermaid images
docker run --rm -v "$PWD:/w" structurizr/structurizr export -w /w/workspace.dsl -f mermaid -o /w/mermaid >/dev/null
mkdir -p images
for f in mermaid/*.mmd; do
    name=$(basename "$f" .mmd)
    name=${name#structurizr-}
    docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/data" minlag/mermaid-cli \
        -i "/data/$f" -o "/data/images/$name.svg" -b white >/dev/null
    echo "images/$name.svg"
done
