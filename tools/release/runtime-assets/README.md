# Extra inline runtime assets (developer integration)

This tool prepares eleven inline arrays missed by SwitchWaker's generated asset stubs: DSP code, debug fonts, and texture/display-list/gradient payloads. It contains only transformation code, disc addresses and payload hashes, not the arrays themselves. Project-original tool under MIT; upstream sources retain their own licences.

Use an **isolated** USA revision-0 source tree. First run upstream `native/tools/gen_asset_stubs.py` to generate the standard 163 runtime stubs/table for that tree. This tool requires that table. It does not configure compiler flags or create a complete game build.

```sh
python3 prepare.py --root /path/to/isolated/SwitchWaker
python3 prepare.py --root /path/to/isolated/SwitchWaker --apply
```

The first command is read-only. Every original payload must match its known hash, and exactly163 base rows must exist before any write. Unrelated source text is preserved. Reapplying is rejected. Apply only to a backed-up isolated tree: validation is performed before writes, but filesystem failures during writes are not a transaction across all files.

Compile the full game/loader consistently with `COS_RUNTIME_ASSETS=1`, zero-content stub includes first, and fresh separate objects. The loader must run before consumers, including static registrations and first-use local arrays. Never mix objects with the embedded-asset build. This tool does not alter build.sh or regenerate commands. Normal upstream registration rules remain in force.

Qualification: eleven real payload hashes checked against original DOL/REL reference bytes; transformed current sources compile in all eight affected ARM translation units. The earlier equivalent isolated build loaded all174arrays byte-identically and reached Outset. Tool host tests cover no-write planning, wrong-version rejection, duplicate rejection and preservation of unrelated edits. This is not console/save qualification or an asset-clearance certificate.

Additional release gates remain in [PLAYER_RELEASE](../../../docs/PLAYER_RELEASE.md): original HOME resources, full matching source/notices, reproducible build, material-cache preparation and gameplay verification.
