# Ghidra function maps

Creates PS2Recomp function-boundary CSVs from a user-supplied DQ8 ELF and
MWo3 overlays. Supports NTSC-U (`SLUS_212.07`, six overlays) and PAL
(`SLES_539.74`, five overlays).

Requires Ghidra with the `ghidra-emotionengine-reloaded` extension, a full JDK
compatible with that Ghidra release, and Python 3. Set `JAVA_HOME` if the JDK
is not on `PATH`.

From the repository root:

```sh
export GHIDRA_HOME=/path/to/ghidra
export ELF=/path/to/extracted/SLUS_212.07
tools/ghidra/run_analysis.sh all
```

The resumable stages are `import`, `analyze`, and `post`. Defaults use `BIN/`
beside the ELF, `build/ghidra` for the project, and `config/<ELF name>` for CSVs.
Override `BIN_DIR`, `PROJECT_DIR`, `PROJECT_NAME`, `OUT_DIR`, `OVERLAYS`,
`LOG_DIR`, `ANALYSIS_TIMEOUT` (seconds), or `MAXMEM` as needed. Exported CSVs
use `Name,Start,End,Size`, with an exclusive end address.

See [Testing](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing)
for validation and [File formats](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/File-Formats)
for overlay layout. Generated maps require review before replacing checked-in maps.
