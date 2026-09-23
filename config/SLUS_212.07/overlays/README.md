# NTSC-U overlays

These recompiler configurations cover the title, casino, viewer, battle, menu,
and shop MWo3 overlays. Each overlay has its own function map and generated
output directory because multiple overlays occupy the same guest address range.

Use `setup.py recompile --version SLUS_212.07 --extracted /path/to/disc-tree`
after building the recompiler. It converts the overlays locally, recompiles them,
and generates the dispatch table.

See [Building](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Building)
and [File Formats](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/File-Formats).
