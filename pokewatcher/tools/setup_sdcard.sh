#!/bin/bash
# Setup an SD card with PokéWatcher Pokemon data.
# Usage: ./setup_sdcard.sh <sd_card_mount_path> <sprite_sheets_dir>
#
# sprite_sheets_dir should contain PNG files named by Pokemon ID (e.g. pikachu.png)
# This script converts them to RGB565 and copies the JSON definitions.

set -e

SDCARD="${1:?Usage: $0 <sd_card_path> <sprites_dir>}"
SPRITES="${2:?Usage: $0 <sd_card_path> <sprites_dir>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Setting up SD card at: $SDCARD"
echo "Sprite sheets from: $SPRITES"

# Create directory structure
mkdir -p "$SDCARD/pokemon"
mkdir -p "$SDCARD/background"

# Copy Pokemon definitions and convert sprites
for pokemon_dir in "$PROJECT_DIR/sdcard/pokemon"/*/; do
    pokemon_id=$(basename "$pokemon_dir")
    echo "Processing: $pokemon_id"

    # Create directory on SD
    mkdir -p "$SDCARD/pokemon/$pokemon_id"

    # Copy JSON files
    cp "$pokemon_dir/pokemon.json" "$SDCARD/pokemon/$pokemon_id/"
    cp "$pokemon_dir/frames.json" "$SDCARD/pokemon/$pokemon_id/"

    # Convert sprite sheet if PNG exists
    png_file="$SPRITES/${pokemon_id}.png"
    if [ -f "$png_file" ]; then
        python3 "$SCRIPT_DIR/convert_sprites.py" "$png_file" "$SDCARD/pokemon/$pokemon_id/overworld.raw"
    else
        echo "  WARNING: No sprite sheet found at $png_file"
    fi
done

echo "Done! SD card is ready."
