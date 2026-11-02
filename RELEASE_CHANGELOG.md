# PZ Mapping Tools - Build 20261101

_Changes since [Build 20261024](https://github.com/Unjammer/PZ_Mapping_Tools/releases/tag/43.00B261024),
published on 2026-10-24._

## Changes

- TileZed and BuildingEd draw map tiles on the graphics card when OpenGL 3.3
  is available. Tile images are packed into texture pages shared by the views
  of a window, and each visible level is drawn in a few batched calls with the
  same order and visibility rules as the raster renderer.
  **Use hardware-accelerated rendering (OpenGL)** in Preferences selects the
  renderer. The raster renderer is used when OpenGL 3.3 is unavailable, when
  the option is cleared, or after starting with `--disable-opengl`. The log
  records the OpenGL vendor, renderer and version.
- The raster renderer splits large redraws, such as zoomed-out views, into
  bands drawn on all processor threads and produces the same pixels as a
  single-threaded redraw. Filtered tiles are reused at 25%, 50% and 75% zoom.
- TileZed and BuildingEd skip map positions outside the area that contains
  tiles on each drawn level, and avoid repeated per-tile text lookups.
  Zoomed-out views of large maps redraw considerably faster with either
  renderer.
- WorldEd draws each visible level of a cell and its adjacent cells through
  shared texture pages in a few batched calls, and recalculates the tilesets
  used by a cell only when its drawn content changes.
- Opening a WorldEd cell with many building lots is faster. A missing tilesheet
  image is searched for once instead of once per placed lot, and is searched
  for again after the Tiles directories change. Placed TBX lots and BuildingEd
  floors index the tilesheet catalogue once per update when applying
  user-drawn tiles, and lot ordering no longer slows down as lots are added.
- Tileset lookups use an index of the Tiles directories that is refreshed when
  those directories change, and tileset images are requested from all image
  loading threads at once.
- `BuildingTiles.txt` and `BuildingFurniture.txt` load faster.
- Render diagnostics refresh their on-screen summary at most four times per
  second.
- WorldEd no longer crashes on exit when OpenGL textures are released without
  an active OpenGL context.
- A Project Zomboid installation in any Steam library is found automatically
  while no installation path is set. Only the default Steam folder was checked
  before, so games installed in another library did not supply the official
  TileDefs, and shaped windows used the default rectangular wall opening in
  all three editors and in generated lots.
- The editors show a warning when no Project Zomboid tile definition file is
  found.

## Credits and special thanks

- Tim Baker for the original WorldEd and TileZed work that remains the
  upstream foundation of these tools.
- Alree / Unjammer for the unofficial Qt 5 continuation, current maintenance,
  new features, fixes, integrations, and releases.
- A very special thank you to Fred 'Military Surplus' Cooper.
- Petro, Pabbiqo [pq], Dane, ! Cacador, Kyber, shakaloblok, shisan-233, and
  the Project Zomboid mapping and modding community for reproducible reports,
  project files, screenshots, logs, and practical workflow feedback.

Legal authorship and third-party attribution are documented in `AUTHORS.txt`,
`FEATURE_PROVENANCE.md`, `UPSTREAM-HISTORY.md`, and the bundled license
notices.
