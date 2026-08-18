# NeoGFF

[![CI](https://github.com/vrifftech/NeoGFF/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoGFF/actions/workflows/ci.yml)

NeoGFF is a native C++17/wxWidgets BioWare GFF editor. It edits classic GFF V3.2 files used by Neverwinter Nights, Neverwinter Nights 2, KotOR, KotOR II, Jade Empire, Dragon Age: Origins, and related games, plus The Witcher GFF V3.3 variant. Recognized classic resources include `.gff`, `.utc`, `.utd`, `.ute`, `.uti`, `.utm`, `.utp`, `.uts`, `.utt`, `.utw`, `.cam`, `.uta`, `.utx`, `.mmd`, `.jrl`, `.dlg`, `.are`, `.git`, `.gic`, `.ifo`, `.pth`, `.fac`, `.gui`, `.sto`, `.cwa`, `.fsm`, `.qst`, `.qst2`, `.itp`, `.bic`, `.btc`, `.bti`, `.btp`, `.btt`, `.cre`, `.pla`, `.trg`, `.cwd`, and `.sav`. It also opens Dragon Age `GFF V4.0PC` / `GFF V4.1PC` files, including `G2DA`/`.gda`, conversations and stages such as `.dlg`, `.cnv`, and `.stg`, cutscenes (`.cut`), plot tables (`.plo`), and morph/path/model/area resources such as `.mor`, `.mop`, `.ani`, `.evt`, `.cl`, `.gad`, `.pwk`, `.mmh`, `.arl`, `.rml`, `.anb`, `.tnt`, and GFF4 `.tlk` resources, with scalar-value save support under the existing V4 schema.


## GUI view

NeoGFF uses a single hierarchical **GFF Tree**

- Structures, lists, and fields appear in their actual document hierarchy.
- Children are populated lazily, so large resources do not create every native tree item at once.
- Double-click an editable field to change its value.
- Add Field and Delete Selected use the currently selected structure, list, or field.
- The filter searches paths, labels, types, stored values, and resolved TLK text.
- **View > Expand GFF Tree** and **View > Collapse GFF Tree** control hierarchy expansion.


## Dragon Age GFF V4 support

NeoGFF detects `GFF V4.0PC` and `GFF V4.1PC` headers and maps V4 numeric labels into readable tree paths where possible. Supported V4 scalar/list payloads include integers, floats, vectors, UTF-16 ECString values, DA2 TLK string references, structs, references, and lists. DA2 `TLK V0.5` files can be opened as GFF4 resources, and the optional TLK resolver can decode their Huffman-compressed UTF-16 string table for resolved StrRef previews. Large DA2 primitive lists, such as ARL terrain/visibility byte arrays, are represented compactly as raw primitive-list fields instead of being expanded into millions of GUI rows.

For GFF V4 files, NeoGFF currently preserves and rewrites the existing template schema and supports scalar value edits. Structural add/delete/rename operations are intentionally rejected for V4 files because V4 struct/field templates are compact schema descriptors rather than the V3.2 string-label tables. Use Neo2DA for spreadsheet-style GDA/G2DA row and column edits.

## Build

This repository consumes shared code from the separate `neoshared` repository. Clone the repositories as siblings:

```text
workspace/
  neoshared/
  NeoGFF/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.


Linux GUI build:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

Windows GUI build with the shared, pinned wxWidgets 3.3.3 overlay:

```powershell
& ..\neoshared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Use `-Wx OFF` on Windows for a CLI/core-only build. The default build directory is `build/`.
