# HD texture packs

HD texture packs replace the game's textures with higher-resolution versions.
Packs are made by players; none ship with this port (the game's
textures belong to its publisher).

## For players

1. Put the pack in the `texture_packs` folder in the game folder (next to
   `TheDarkness.exe`). Create the folder if it's missing. Each pack can stay in
   its own subfolder, for example `texture_packs\Street Pack\...`; several
   packs can sit side by side.
2. Turn on **Graphics Quality > HD texture packs** in the launcher, or in the
   in-game settings (F1). In game the switch is marked *next start*: it takes
   effect the next time the game starts.

The switch is greyed out while no pack is installed. With a pack installed the
settings show what it costs:

- **Disk space**: the size of the pack's files.
- **Video memory**: the whole pack stays in the graphics card's memory for the
  session (each texture is its own allocation of at least 64 KB). The setting
  compares this with your card's memory and warns when a pack takes more than a
  quarter of it; if the game stutters or fails to load the pack, lower the
  internal scale or turn packs off.
- **Loading**: the pack loads in the background from the first frame, at most
  16 files at a time and 8 MB of copies per frame, so the game never waits for
  it. Until a replacement is ready the original texture shows. Measured: a
  540 MB pack was fully on the graphics card 1.65 seconds after start (NVMe
  SSD, a current graphics card); a hard disk takes longer. Gameplay frame times with the
  pack loaded matched runs without it.

With the switch off nothing is scanned or loaded: the game looks and runs
exactly as without packs (checked: no pack activity in the log even with a pack
installed, and the busiest street runs at the same frame rate as the build
before pack support).

If two packs contain the same texture, the first file found wins and the log
names the other one (`REX_TEXTURE_PACK_DUPLICATE`).

## For pack makers

### Texture ids

A replacement file is named after the original texture's content id: 16
hexadecimal digits, for example `3f2a9c0d11b84e57.dds`. The id is a hash of
the texture's description (format, size, mip levels, tiling) and its data as
the game loaded it, so it is the same on every PC and in every session, and
does not depend on where the texture sits in memory.

### Dumping the originals

Start the game with the environment variable `REX_GPU_TEXTURE_DUMP=true`, for
example from a command prompt in the game folder:

```bat
set REX_GPU_TEXTURE_DUMP=true
TheDarkness.exe
```

Every texture the game loads is written once to the `texture_dump` folder in
the game folder: `<id>.dds` (the texture as the graphics card sees it, all mip
levels) and `<id>.json` (its original format and size). Play through the areas
you want to cover; textures already dumped in an earlier session are skipped.
`REX_GPU_TEXTURE_DUMP_ROOT` chooses another dump folder.

### Making a replacement

- Keep the file name (the id) and the **aspect ratio** of the original; the size
  can be any multiple (2x, 4x, ...), up to 16384 x 16384.
- Keep the color space of the dump. The game applies its own gamma, so a
  replacement is read as plain values: an sRGB-tagged file is read as if it
  were not tagged.
- Include a full mip chain; the game samples distant surfaces from small mips.
- DDS files only, 2D textures with one slice (no cube maps, volumes or arrays).
  Formats: BC1-BC7 (DX10 header, or legacy DXT1-DXT5, ATI1/BC4U, ATI2/BC5U),
  R8G8B8A8, B8G8R8A8/X8, R10G10B10A2, R11G11B10, R16G16, R16G16B16A16, R8G8,
  R16, R8, A8, B5G6R5, B5G5R5A1, B4G4R4A4. BC7 or BC1/BC3 keep video memory
  low; uncompressed RGBA8 takes 4-8 times more.
- Signed textures (for example normal maps the game stores signed) use the
  signed twin of the file's format where one exists (BC4, BC5, R8G8B8A8, R8G8,
  R8, R16G16B16A16).

Textures the game rewrites while playing (render targets, videos, animated
surfaces) change id every time they change, so they can't be replaced; when
the game rewrites a replaced texture, the original comes back automatically.

### Checking a pack

The game log (`stderr`) reports the pack at start:

- `REX_TEXTURE_PACK ... files=N disk_mb=... vram_mb=... rejected=R duplicates=D`
- `REX_TEXTURE_PACK_REJECTED file=... reason=...` for each file it can't use
- `REX_TEXTURE_PACK_PRELOADED ... seconds=S` once the whole pack is loaded
- `REX_TEXTURE_PACK_REPLACED textures=N ...` as textures switch over

`REX_GPU_TEXTURE_PACK_ROOT` reads packs from another folder (for testing a
pack without installing it).
