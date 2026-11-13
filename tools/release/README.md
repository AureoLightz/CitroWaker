# Release preparation checks

The existing developer CIA is **not** an asset-free public release. The current build includes byte arrays generated from the disc, embedded bottom-screen game artwork and Nintendo-derived HOME resources/music. The linked DSP HLE also requires a complete corresponding-source/licence arrangement for binary distribution.

`audit_assets.py` demonstrates one part of that issue without uploading the assets:

```sh
python3 audit_assets.py --elf /private/build/ww3ds.elf \
  --asset-headers /private/build/assets/include/assets --output /private/audit.json
```

Exit 1 means an exact match to known raw asset bytes was found. Exit 2 is an invalid/incomplete audit input. Exit 0 only means no checked raw-byte initializer matched: it is **not** a public-release approval. Floats, compressed code, other embedded artwork, firmware, notices and source completeness need separate review. Supply the ELF paired with the CIA; searching a compressed CIA alone is insufficient. Output includes hashes/locations, not the asset bytes or absolute input paths. Review source-header names before sharing any report.

## Public player release requirements

- Compile with runtime asset stubs and fill them from the player's own ISO, or build the asset-bearing CIA locally. Upstream already has `gen_asset_stubs.py` and `pc_assets.cpp`; the current 3DS build is not wired for that mode.
- Use original embedded artwork/music, or load player-derived presentation assets locally. Current private Master Sword/title-music resources are not redistributable source assets.
- Publish the full matching source/build changes and relevant notices for the linked GPL DSP HLE and other dependencies. The MIT component subset alone does not meet that goal.
- Supply or generate matching material caches locally. The current public setup still needs these and a compatible CIA; texture extraction alone cannot produce them.
- Test a fresh SD tree from the player's own ISO, including boot, saving, updates and the required console memory mode. Preserve saves/configuration during updates.
- Distribute a checksummed versioned release with accurate hardware compatibility, known issues and source links. Do not label the existing developer CIA ready based on a filename or an asset-scan pass.

These checks concern the release artifact. The independently usable public tools/source remain available.
