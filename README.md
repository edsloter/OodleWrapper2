###############################################

Modified version of UnrealOodleWrapper
see full readme for all changes. 
added `-` as shorthand for stdin/stdout
added parallelism with official SDK methods, see below
###############################################

# UnrealOodleWrapper
Visual Studio project for linking Unreal Engine Oodle plugin with command tool

Release nor library are included since Oodle is a licensed property of Epic Games Inc. that cannot be shared publicly.

Download Unreal Engine 4.27+ from Epic Launcher (not tested with 5),
copy "include" and "lib" folders from Oodle plugin's sdk to root folder of project. 

Default Oodle sdk path: 
```
X:\Program Files\Epic Games\UE_4.27\Engine\Plugins\Compression\OodleData\Sdks\2.9.0
```
and compile in Visual Studio.

Used setup:
- Visual Studio 2022 (v143)
- Windows SDK 10.0.19041.0

For additional info while decompressing, like what type of compression method was used, you must run the tool under a debugger.

# Help
```
UnrealOodleWrapper usage

```
.exe [option] <input> <output>
Option:
    -c <N> <M>    Compress. N = compression level (-4..9). M = method (-1..5).
                  Optional flags: `--legacy` (emit raw compressed bytes, no OOZ header) and
    -d <N>        Decompress. N = decompressed size in bytes (optional when header present).

    - `--legacy` (emit raw compressed bytes, no OOZ header)
      `--ooz` (emit simple 8-byte original-size prefix for compatibility)
      `--jobify <mode>` to control internal parallel jobs (modes: default, disable, normal, aggressive).
      `--jobify-count <N>` can be used to set the target parallelism (overrides env var `UNREALOODLE_JOB_THREADS`).
Special paths:
    Use `-` as shorthand for stdin/stdout. Example: `.exe -c 9 3 - - > out.oodle` compresses from stdin to stdout.
    You can also use `stdin` (streaming) or `stdin=N` (legacy fixed-size stdin).
    When writing output, `-` or `stdout` may be used to write to stdout.

Notes on auto-detection and compatibility:
    - The tool now always checks for an OOZ header in compressed data and will use it if present (so `-d` does not need to be 0 to trigger detection).
    - By default compressed output includes an 8-byte OOZ-style header (4B magic + 32-bit LE original size). This allows `-d 0` or any `-d N` to work when the header is present.
    - If the compressed data does not contain an OOZ header, the tool will attempt to build an Oodle seek-table to determine the original size. Very small or certain compressed streams may not yield a valid seek-table; in those cases provide the exact decompressed size to `-d`.
    - For legacy compatibility the tool supports a `--legacy` compress option which suppresses writing the OOZ header and emits only the raw compressed bytes. Decompression of such legacy files requires either providing the decompressed size or relying on seek-table detection.
    - The `--ooz` option emits a simple 8-byte compatibility header (an unsigned 64-bit little-endian original size) immediately followed by the raw compressed payload. This is provided for compatibility with older tools that expect an original-size prefix. Unlike the default SDK-style OOZ header, `--ooz` does not include SDK header fields or a seek-table.

Compression Levels:
    <0 = bias towards speed
     0 = no compression (copy)
    >0 = bias towards compression ratio

Compression Methods:
    -1 = LZB16 (DEPRECATED but still supported)
     0 = None (pass-through)
     1 = Kraken
     2 = Leviathan
     3 = Mermaid
     4 = Selkie
     5 = Hydra
```

# Examples

- decompress file `test.oodle` that decompressed has size `5578` B and save it as `test.unc`
```
.exe -d 5578 test.oodle test.unc
```
- compress file `test.temp` using `Mermaid method` with `compression level 9` and save it as `test.oodle`
```
.exe -c 9 3 test.temp test.oodle
```
