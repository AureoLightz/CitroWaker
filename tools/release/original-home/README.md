# Original release HOME resources

These generators use no disc assets or sampled audio. The open-ring/wind emblem, ocean gradient and CitroWaker lettering are original project artwork. No sword, Triforce, game textures or game font is included. Original code/generated artwork/audio under MIT.

Artwork needs Pillow and an installed font, default DejaVu Serif Bold (pass `--font` for another appropriately licensed font). No font file is bundled. `python3 generate.py --output /private/art` produces a256x128banner,48/24pixel icons and larger previews. `python3 generate_audio.py` writes the2.7second stereo32kHz oscillator bell beside the script.

Feed those images/sound to bannertool's standard image-banner path; do not use the old custom CGFX. The isolated candidate successfully packages with the new resources and RSF Logo field omitted: ctrtool reports logo size0 and New3DS124MB. This is packaging inspection, not a real HOME Menu compatibility test. Existing private game-derived artwork remains separate.

Do not mistake original HOME resources for a complete asset audit of an executable.
