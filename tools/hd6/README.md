# HD6 archive extractor

Unpacks a user-supplied `DATA.DAT` or `DATA2.DAT` using its paired HD6 index.
Uses Python 3's standard library.

```sh
python3 tools/hd6/hd6_extract.py --hd6 /path/to/DATA.HD6 \
    --dat /path/to/DATA.DAT --out /path/to/assets \
    --manifest /path/to/manifest.json
python3 tools/hd6/hd6_extract.py --hd6 /path/to/DATA.HD6 --list-only
```

Paths are sanitized before extraction. Sentinel entries are skipped; dummy
entries are recorded without writing their padding bytes. The JSON manifest
records names, offsets, sizes, SHA-1 hashes, and extraction totals. Output
retains the disc's stored alignment padding.

See [File formats](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/File-Formats)
for the archive layout and [Testing](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/Testing)
for validation. Extracted assets stay outside the source distribution.
