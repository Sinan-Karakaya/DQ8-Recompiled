# MVI converter

Converts local MVI movies to MPEG program streams using a user-supplied XOR key.

```sh
python3 tools/mvi/mvi_decrypt.py --in /path/to/MOVIE --out /path/to/movies --key /path/to/key.bin
```

The output preserves the input directory structure and uses the `.pss` extension.
No key or movie data is distributed. This is an offline inspection tool; the game
runtime reads movies from the supplied disc image.

See [File Formats](https://github.com/Sinan-Karakaya/DQ8-Recompiled/wiki/File-Formats)
for the container layout.
