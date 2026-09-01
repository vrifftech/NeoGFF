# NeoGFF

[![CI](https://github.com/vrifftech/NeoGFF/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoGFF/actions/workflows/ci.yml)

NeoGFF is a BioWare GFF editor. It edits classic GFF V3.2 files used by Neverwinter Nights, Neverwinter Nights 2, KotOR, KotOR II, Jade Empire, Dragon Age: Origins, and related games, plus The Witcher GFF V3.3 variant. Recognized classic resources include `.gff`, `.utc`, `.utd`, `.ute`, `.uti`, `.utm`, `.utp`, `.uts`, `.utt`, `.utw`, `.cam`, `.uta`, `.utx`, `.mmd`, `.jrl`, `.dlg`, `.are`, `.git`, `.gic`, `.ifo`, `.pth`, `.fac`, `.gui`, `.sto`, `.cwa`, `.fsm`, `.qst`, `.qst2`, `.itp`, `.bic`, `.btc`, `.bti`, `.btp`, `.btt`, `.cre`, `.pla`, `.trg`, `.cwd`, and `.sav`. It also opens Dragon Age `GFF V4.0PC` / `GFF V4.1PC` files, including `G2DA`/`.gda`, conversations and stages such as `.dlg`, `.cnv`, and `.stg`, cutscenes (`.cut`), plot tables (`.plo`), and morph/path/model/area resources such as `.mor`, `.mop`, `.ani`, `.evt`, `.cl`, `.gad`, `.pwk`, `.mmh`, `.arl`, `.rml`, `.anb`, `.tnt`, and GFF4 `.tlk` resources.


## GUI view

NeoGFF uses a single hierarchical **GFF Tree**

- Structures, lists, and fields appear in their actual document hierarchy.
- Children are populated lazily, so large resources do not create every native tree item at once.
- Double-click an editable field to change its value.
- Add Field and Delete Selected use the currently selected structure, list, or field.
- The filter searches paths, labels, types, stored values, and resolved TLK text.
- **View > Expand GFF Tree** and **View > Collapse GFF Tree** control hierarchy expansion.
- Each open document keeps its own expanded branches, selected field, filter text, and tree position when switching between tabs.


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


Windows GUI build:

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

## TSLPatcher/HoloPatcher output

Generate GFF patcher instructions from an original GFF and a modified GFF:

The GUI offers two output choices. **Write to INI** asks you to select or create an installer INI and merges the new `[GFFList]`, file-section, and `AddFieldN` instructions without overwriting unrelated content. **Fragment** opens a read-only preview and lets you copy the generated INI sections to the clipboard or save that exact text as a new INI file. It never merges into an existing INI and does not stage the clean baseline GFF. Use any `.ini` name for separate install options. The CLI retains file-based `--package`, `--fragment`, and `--ini install_full.ini` options.

Colliding `FileN`/`AddFieldN` keys, generated helper-section names, `2DAMEMORY#` tokens, and `StrRef#` tokens are remapped automatically. Identical baseline assets are retained. A different existing payload with the same filename is rejected rather than overwritten.

Editable scalar and localized-string changes become direct field assignments under `[GFFList]`. Added fields become `AddFieldN` sections. Deleted fields, type changes, and structural reorders are reported as unsupported by default.

Patcher generation accepts XML, JSON, or native classic GFF V3.2 data for the common KotOR-style resource types supported by both original TSLPatcher and HoloPatcher 1.7. DLG resources must use NeoDLG. Jade Empire resources, `JadeStringRef`, Witcher GFF V3.3, and Dragon Age GFF4 are rejected from patcher export even though NeoGFF can edit them natively.

Patcher export is limited to matching GFF V3 documents. Generic NeoGFF output rejects GFF V4 and native `DLG` files; use NeoDLG for dialogue-aware graph patching. The GUI provides one patcher export command under **Export**, followed by a **Write to INI** or **Fragment** choice.

## Shared game directories

The wxWidgets application exposes **File > Open Game Directory**. Its submenu lists every saved game install from the shared `neoshared` settings store; selecting an entry opens this application's supported-file dialog with that installation as the starting folder. **Manage Game Directories...** adds, renames, rescans, activates, or removes shared entries, and changes are visible in every Neo tool.