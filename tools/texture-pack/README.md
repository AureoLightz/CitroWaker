# PC texture preparation

This is the port's actual offline texture converter, with a portable disc extractor. It reads the player's USA GameCube ISO and produces the `ww_texpack.bin` understood by compatible builds. No game texture or generated pack is included.

## Build and run

Python 3.10+ and a C++17 GCC/Clang compiler are required. On Windows use MinGW-w64 or a suitable Clang toolchain; no Windows binary/test result is claimed yet.

```sh
python3 build.py
python3 extract_textures.py /path/to/GZLE01.iso /private/work/texsrc.bin
./build/mktexpack /private/work/texsrc.bin /private/work/ww_texpack.bin --threads 2 --verify
```

Windows: use `py -3` and `build\mktexpack.exe`. Build source needs only the C++ standard library/threads, not the 3DS SDK, renderer executable, an emulator or a developer-specific home directory. `CXX` or `--cxx` selects the compiler executable.

The texture pack belongs in `/3ds/ww3ds/` beside the player's ISO. It does **not** contain the executable, material bundle, model pack or a complete SD export. Build compatibility must be checked; do not substitute it for a matching release manifest.

The extractor walks the FST, Yaz0/RARC archives, J3D TEX1 blocks, JPC textures and BTIs. The converter uses the same format-selection/error policy as the port, writes pack version 2, and `--verify` reads it back using the matching reader. That verifies stored converted bytes; it does not imply exact original GameCube sampling or remove existing runtime approximation policies. Some unexpressible binding variants retain the game's runtime fallback.

Outputs are local game-derived data: do not commit or redistribute them. Allow at least 1 GiB of working space; keep input and output paths distinct. Both tools refuse existing output paths. The converter validates input sizes before allocating, caps input at 256 MiB and caps workers at four (default two). Use a new private work directory.

## Qualification

```sh
python3 -m unittest -v test_extract.py test_converter.py
```

Extractor tests use synthetic BTI/J3D/Yaz0 input. Native converter tests use synthetic texture records, check `--verify` and compare single/two-worker outputs. They run after building; otherwise the native test is explicitly skipped. Linux is qualified; Windows remains untested.

A local real-ISO check extracted 5,049 unique textures with no reported bad records. An 84-texture sample spanning 11 source formats produced 554 verified pack entries, byte-identical to the existing developer converter. The disc/sample/output are not published. This is not a full-game runtime validation or a measured FPS gain.

## Provenance and notices

The eight texconv/texmap/texfit/texpack implementation/header files are unchanged project-original snapshots. The CLI wrapper adds size/version checks, bounded workers and exclusive outputs without changing conversion policy. The narrow binding/hash support copies the exact type/helper bodies without linking the rest of the renderer. See `PROVENANCE.json`.

`third_party/switchwaker/disc_manifest.py` is the unmodified parser from SwitchWaker at the recorded commit, under its adjacent MIT notice. The extractor imports this colocated parser, replacing a developer-specific path. Its publication wrapper also adds help/revision validation and refuses to replace an input or existing output/listing; extraction/serialization are unchanged. Root MIT covers the project-original files, not Nintendo data read at runtime. Preserve the upstream notice when distributing the parser.
