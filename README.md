# NeoGFF

[![CI](https://github.com/vrifftech/NeoGFF/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoGFF/actions/workflows/ci.yml)

NeoGFF is a native C++17/wxWidgets BioWare GFF editor. It edits classic GFF V3.2 files used by Neverwinter Nights, Neverwinter Nights 2, KotOR, KotOR II, Jade Empire, Dragon Age: Origins, and related games, plus The Witcher GFF V3.3 variant. Recognized classic resources include `.gff`, `.utc`, `.utd`, `.ute`, `.uti`, `.utm`, `.utp`, `.uts`, `.utt`, `.utw`, `.cam`, `.uta`, `.utx`, `.mmd`, `.jrl`, `.dlg`, `.are`, `.git`, `.gic`, `.ifo`, `.pth`, `.fac`, `.gui`, `.sto`, `.cwa`, `.fsm`, `.qst`, `.qst2`, `.itp`, `.bic`, `.btc`, `.bti`, `.btp`, `.btt`, `.cre`, `.pla`, `.trg`, `.cwd`, and `.sav`. It also opens Dragon Age `GFF V4.0PC` / `GFF V4.1PC` files, including `G2DA`/`.gda`, conversations and stages such as `.dlg`, `.cnv`, and `.stg`, cutscenes (`.cut`), plot tables (`.plo`), and morph/path/model/area resources such as `.mor`, `.mop`, `.ani`, `.evt`, `.cl`, `.gad`, `.pwk`, `.mmh`, `.arl`, `.rml`, `.anb`, `.tnt`, and GFF4 `.tlk` resources, with scalar-value save support under the existing V4 schema.

The Open and Save As dialogs include a dedicated Jade Empire GFF filter for `.gff`, `.are`, `.gui`, `.sto`, `.cwa`, `.qst`, `.qst2`, `.pla`, `.cre`, `.trg`, `.dlg`, `.fsm`, `.cwd`, and `.sav`. New documents also choose the corresponding extension from their four-byte GFF content type. Jade `.lyt`, `.vis`, `.art`, `.bip`, `.amp`, and `.ndb` resources are not GFF files and are intentionally not presented as NeoGFF formats. Purpose-aware QST/QST2 editing belongs to NeoQST; NeoGFF remains the generic structural editor.

The Witcher `.wfx` effect format and Neverwinter Nights 2 `NWN2`-header `.rim`/`.trx` terrain containers are not GFF resources and are intentionally excluded from NeoGFF's format list.

The GUI executable is named `NeoGFF`; the command-line utility is named `neogff-cli` to avoid filename collisions on case-insensitive filesystems.


## GUI views

The GUI has two GFF element views:

- **Flat Grid View** shows the existing path/label/type/value table and remains the default for direct value editing.
- **Element Tree View** shows the same GFF elements as a hierarchical tree, similar to K-GFF's tree navigation, with expand/collapse commands in the View menu. Selecting a tree item also selects the corresponding model row so Add Field, Delete Selected, copy, paste, and value editing continue to use the same data model. Filtered trees keep placeholder parents when needed, and localized-string child entries are grouped under their owning field.

Choose **View > Flat Grid View** or **View > Element Tree View**. The selected view is remembered through the application's wxConfig settings and restored on the next launch.


## Dragon Age GFF V4 support

NeoGFF detects `GFF V4.0PC` and `GFF V4.1PC` headers and maps V4 numeric labels into readable tree paths where possible. Supported V4 scalar/list payloads include integers, floats, vectors, UTF-16 ECString values, DA2 TLK string references, structs, references, and lists. DA2 `TLK V0.5` files can be opened as GFF4 resources, and the optional TLK resolver can decode their Huffman-compressed UTF-16 string table for resolved StrRef previews. Large DA2 primitive lists, such as ARL terrain/visibility byte arrays, are represented compactly as raw primitive-list fields instead of being expanded into millions of GUI rows.

For GFF V4 files, NeoGFF currently preserves and rewrites the existing template schema and supports scalar value edits. Structural add/delete/rename operations are intentionally rejected for V4 files because V4 struct/field templates are compact schema descriptors rather than the V3.2 string-label tables. Use Neo2DA for spreadsheet-style GDA/G2DA row and column edits.

## CLI usage

```text
neogff-cli info <gff> [--tlk dialog.tlk]
neogff-cli dump <gff> [filter-term] [--tlk dialog.tlk]
neogff-cli search <gff> <term> [--tlk dialog.tlk]
neogff-cli roundtrip <input-gff> <output-gff>
neogff-cli new <output-gff> [file-type]
neogff-cli set-value <input-gff> <output-gff> <path> <value>
neogff-cli add-field <input-gff> <output-gff> <parent-path|.> <label> <type> [value] [struct-type-id]
neogff-cli delete-field <input-gff> <output-gff> <path>
```

Paths use GFF labels separated by backslashes. List entries are addressed by numeric index, for example:

```text
InventoryList\0\Item
```

Jade Empire SAV resources can repeat the same field label within one struct. NeoGFF displays and addresses those occurrences explicitly:

```text
CreatureList\StaticList\0\AI\LastHeartbeatScr[#1]
CreatureList\StaticList\0\AI\LastHeartbeatScr[#2]
```

The occurrence suffix is one-based and is only shown when a label is duplicated in that struct.

Localized strings expose editable child paths:

```text
LocalizedName(strref)
LocalizedName(lang0)
```

Supported field types are:

```text
Byte, Char, Word, Short, DWORD, Int, DWORD64, Int64, Float, Double,
CExoString, CResRef, CExoLocString, Void, Struct, List, Orientation, Position, JadeStringRef
```

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

## Tabular import/export

`neogff-cli` can `search`, `export`, and `import` semantic XML and JSON. `info`, `dump`, and `search` also accept `--tlk dialog.tlk` to show resolved TLK text for CExoLocString StrRefs, Jade/DA2 string-reference fields, and obvious StrRef numeric fields. TLK loading is optional. Classic TLK `V3.0`/`V4.0` and DA2 GFF4 `TLK V0.5` are supported for lookup. The GUI remembers the last TLK opened with **Open optional TLK...** and attempts to auto-load it on the next launch so StrRefs resolve automatically, but files open and edit without one. XML and JSON use complete hierarchical typed GFF documents (`<gff3 type=...><struct id=...>...</struct></gff3>` for XML, and the analogous semantic JSON tree). CSV/TSV flattened GFF value-table import/export is intentionally not exposed because it does not preserve the semantic structure of GFF data. The GUI provides **Open optional TLK**, **Import XML**, **Import JSON**, **Export XML**, **Export JSON**, filter/search, and cell copy/paste actions for editable value rows. Hierarchical XML/JSON export is intentionally unfiltered so it remains a complete GFF document.

## TSLPatcher/HoloPatcher output

Generate GFF patcher instructions from an original GFF and a modified GFF:

```sh
neogff-cli diff-tslpatcher original.utc modified.utc tslpatchdata --package --filename edited.utc
neogff-cli diff-tslpatcher original.utc modified.utc gff_fragment.ini --fragment --filename edited.utc
```

Editable scalar and localized-string changes become direct field assignments under `[GFFList]`. Added fields become `AddFieldN` sections. Deleted fields, type changes, and structural reorders are reported as unsupported by default.

Patcher generation accepts imported modified-side GFF data: `--modified-format xml|json|gff|kotor|native|auto` or a known native GFF extension alias such as `gff`, `utc`, `dlg`, `jrl`, `qst2`, `sto`, `fsm`, `cwa`, `cre`, `pla`, or `trg`; `diff-tslpatcher-import` accepts the same formats. XML/JSON are full hierarchical GFF documents; native GFF files can also be compared directly.

Patcher export is limited to matching GFF V3 documents. Generic NeoGFF output rejects GFF V4 and native `DLG` files; use NeoDLG for dialogue-aware graph patching. The GUI provides package and fragment export commands under **Export**.

## Shared game directories

The wxWidgets application exposes **File > Open Game Directory**. Its submenu lists every saved game install from the shared `neoshared` settings store; selecting an entry opens this application's supported-file dialog with that installation as the starting folder. **Manage Game Directories...** adds, renames, rescans, activates, or removes shared entries, and changes are visible in every Neo tool.

## Continuous integration

GitHub Actions checks out `vrifftech/neoshared` beside this repository, then builds the full wxWidgets application on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. Successful non-pull-request runs publish staged Linux and Windows artifacts.

The shared dependency defaults to `neoshared/main`. Set the repository Actions variable `NEOSHARED_REF` to a release tag or commit SHA to pin normal CI builds. A manual workflow run can override the ref, and the workflow accepts the `neoshared-updated` repository-dispatch event for cross-repository compatibility checks.
