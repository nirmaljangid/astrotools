# Astro Toolkit

A single-binary Qt6 desktop application combining a deep-sky **target planner**, a **FITS image reviewer** with auto-quality-check, and a **FITS processing pipeline** (sort → arrange → stack with Siril).

---

## Table of Contents

1. [Features](#features)
2. [Dependencies](#dependencies)
3. [Build](#build)
4. [First-run setup](#first-run-setup)
5. [Configuration](#configuration)
6. [Gear profiles](#gear-profiles)
7. [Tab-by-tab guide](#tab-by-tab-guide)
8. [Keyboard shortcuts](#keyboard-shortcuts)
9. [Auto Review — how it works](#auto-review--how-it-works)
10. [Install / uninstall](#install--uninstall)

---

## Features

| Area | What it does |
|------|-------------|
| **Target Planner** | Queries a local SQLite NGC/IC/Messier catalogue, filters objects visible tonight from your location, scores and ranks them by altitude window, FOV fit, and object type |
| **Sky chart** | Altitude-vs-time arc plot for the selected object |
| **Framing helper** | DSS sky survey overlay with your sensor footprint drawn to scale |
| **AI imaging plan** | Calls OpenAI to generate a per-object imaging plan (exposure times, filter sets, stacking advice) |
| **FITS Reviewer** | Browse any folder of FITS files, view with four stretch modes, delete bad frames manually or via Auto Review |
| **Auto Review** | Scans all loaded frames, detects out-of-focus images, star trails, and frames with fewer than 10 detected stars; flags them for batch deletion |
| **Sort FITS** | Sorts raw frames into `Object/Filter/[Exposure]/` folder trees using cfitsio header data |
| **Arrange for Stacking** | Matches calibration frames (bias, dark, flat) to light frames by gain, exposure, and sensor dimensions |
| **Stack with Siril** | Generates Siril scripts and runs stacking end-to-end from within the app |

---

## Dependencies

### Required

| Library | Min version | Arch Linux | Ubuntu / Debian | Fedora |
|---------|-------------|------------|-----------------|--------|
| **Qt6** (Core, Gui, Widgets, Concurrent, Sql) | 6.2 | `qt6-base` | `qt6-base-dev` | `qt6-qtbase-devel` |
| **libcurl** | any | `curl` | `libcurl4-openssl-dev` | `libcurl-devel` |
| **cfitsio** | any | `cfitsio` | `libcfitsio-dev` | `cfitsio-devel` |
| **nlohmann/json** | 3.x | `nlohmann-json` | `nlohmann-json3-dev` | `nlohmann-json-devel` |
| **cmake** | 3.20 | `cmake` | `cmake` | `cmake` |
| **pkg-config** | any | `pkgconf` | `pkg-config` | `pkgconf` |

### Optional

| Library | Purpose | Arch | Ubuntu | Fedora |
|---------|---------|------|--------|--------|
| **gpsd** | GPS-based location | `gpsd` | `libgps-dev` | `gpsd-devel` |
| **Siril** | In-app stacking | `siril` | `siril` | `siril` |

### One-liner installs

**Arch Linux**
```bash
sudo pacman -S qt6-base curl cfitsio nlohmann-json cmake pkgconf gpsd siril
```

**Ubuntu / Debian**
```bash
sudo apt-get install qt6-base-dev libcurl4-openssl-dev libcfitsio-dev \
    nlohmann-json3-dev cmake pkg-config libgps-dev siril
```

**Fedora**
```bash
sudo dnf install qt6-qtbase-devel libcurl-devel cfitsio-devel \
    nlohmann-json-devel cmake pkgconf gpsd-devel siril
```

---

## Build

```bash
git clone <repo-url> combined_astro
cd combined_astro

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The binary is produced at `build/astro_toolkit`.

### Debug build (with AddressSanitizer + UBSan)

```bash
cmake -B build_dbg -DCMAKE_BUILD_TYPE=Debug
cmake --build build_dbg -j$(nproc)
```

### Cross-compile / portable build (no `-march=native`)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CROSSCOMPILING=ON
cmake --build build -j$(nproc)
```

---

## First-run setup

### 1. Build the object catalogue

The planner needs a local SQLite database built from the OpenNGC data files.  
Clone [OpenNGC](https://github.com/mattiaverga/OpenNGC) and run the bundled importer:

```bash
# Download OpenNGC CSV files
wget https://raw.githubusercontent.com/mattiaverga/OpenNGC/master/database/NGC.csv
wget https://raw.githubusercontent.com/mattiaverga/OpenNGC/master/database/addendum.csv

# Build the SQLite catalogue
python import_opengnc_sqlite.py \
    --db ~/.astro_gui/catalog.sqlite \
    --ngc NGC.csv \
    --addendum addendum.csv \
    --rebuild-aliases
```

The app will show an error on startup if the database file is missing and print the exact command to run.

### 2. Run the application

```bash
./build/astro_toolkit
```

The app creates `~/.astro_gui/config.json` with defaults on first launch.

---

## Configuration

Edit `~/.astro_gui/config.json` (created automatically on first run):

```json
{
  "sqlite_db_path":   "/home/you/.astro_gui/catalog.sqlite",
  "openai_api_key":   "sk-...",
  "openai_model":     "gpt-4o-mini",
  "ip_geo_url":       "https://ipapi.co/json/",
  "min_alt_deg":      30.0,
  "min_hours_above":  4.0,
  "page_size":        5,
  "active_profile":   0,
  "gear_profiles":    []
}
```

| Key | Default | Description |
|-----|---------|-------------|
| `sqlite_db_path` | `~/.astro_gui/catalog.sqlite` | Path to the NGC catalogue database |
| `openai_api_key` | _(empty)_ | OpenAI API key for AI imaging plans (optional) |
| `openai_model` | `gpt-4o-mini` | OpenAI model to use |
| `ip_geo_url` | ipapi.co | Endpoint for IP-based location fallback |
| `min_alt_deg` | `30` | Minimum object altitude in degrees to be listed |
| `min_hours_above` | `4` | Minimum hours the object must be above `min_alt_deg` |
| `page_size` | `5` | Objects loaded per "Load More" click |

### Location detection

Location is resolved in order:

1. **gpsd** — if the daemon is running and has a fix (requires `libgps` at build time)
2. **IP geolocation** — falls back to the `ip_geo_url` endpoint
3. Manual — edit `config.json` to add a static `lat`/`lon` if neither works

---

## Gear profiles

Gear profiles store your telescope and camera parameters. They are managed through the **⚙ Profiles** button in the toolbar or via `config.json`.

Each profile contains:

| Field | Description |
|-------|-------------|
| `name` | Display name |
| `scope_type` | Newtonian / Refractor / SCT / RC / Mak-Cas / Mak-New / Dobsonian |
| `focal_length_mm` | Focal length in mm |
| `aperture_mm` | Aperture in mm |
| `camera_name` | Camera label |
| `pixel_um` | Pixel size in µm |
| `width_px` / `height_px` | Sensor resolution |
| `has_lrgb`, `has_ha`, `has_oiii`, `has_sii` | Available filter sets |

The active profile drives the FOV indicator, framing overlay, and the AI imaging plan prompt.  
Several built-in presets are available in the profile dialog as a starting point.

---

## Tab-by-tab guide

### FITS Reviewer _(tab 0)_

1. Click **Browse…** or paste a folder path and press **Open** (or Enter).
2. The left panel lists all `.fit/.fits/.fts` files found recursively.
3. Click a file or use **◀ Prev / Next ▶** (or keyboard shortcuts) to navigate.
4. Use the **Stretch** combo to switch between `linear`, `sqrt`, `log`, `asinh`.
5. Click **Delete File** (or press `x` / `Del`) to permanently delete the current frame after confirmation.
6. Click **⚡ Auto Review…** to run the automated quality check on all loaded files (see [Auto Review](#auto-review--how-it-works)).

### Tools _(tab 1)_

Three sub-tabs in workflow order:

#### Sort FITS by Object / Filter
- Set **Source** folder (raw captured frames) and **Destination** folder.
- Optionally enable **Split by exposure time** or **Dry run** (preview without moving files).
- Output structure: `Destination/ObjectName/FilterName/[ExposureTime]/filename.fit`
- Calibration frames are detected automatically by `IMAGETYP` and sorted into `bias/`, `dark/`, `flat/` sub-folders.

#### Arrange for Stacking
- Point to the sorted folder produced by the previous step.
- The app matches calibration frames to each set of lights by gain (±15%), exposure (±10%), and exact sensor dimensions.
- Produces a structured folder ready for Siril.

#### Stack with Siril
- Reads the arranged folder, generates Siril `.ssf` scripts for each target/filter combination, and runs `siril-cli` automatically.
- Live log output is shown in the panel.
- Requires `siril-cli` to be in `PATH`.

### Target Planner tabs _(tabs 2–6)_

| Tab | Content |
|-----|---------|
| Nebula (Ha/SHO) | Emission nebulae, scored for narrowband imaging |
| Galaxies | Galaxy catalogue |
| Star clusters | Open and globular clusters |
| Messier | Full Messier catalogue |
| Search | Free-text search by name or catalogue ID (e.g. `M42`, `NGC7000`, `IC1805`) |

- Objects are filtered by tonight's dark window, minimum altitude, and minimum visible hours.
- With a gear profile set, objects are also filtered by FOV fit (20%–90% of the long sensor axis).
- Click any object to see its sky-path chart, full details, and an AI-generated imaging plan.
- Switch the right panel between **Sky Chart**, **Details**, and **Framing** using the buttons at the bottom.

---

## Keyboard shortcuts

These work when the FITS viewer canvas has focus (click on the image first):

| Key | Action |
|-----|--------|
| `→` `l` `n` | Next file |
| `←` `h` `p` | Previous file |
| `s` | Cycle stretch mode |
| `x` `Del` | Delete current file (asks for confirmation) |

---

## Auto Review — how it works

The **⚡ Auto Review…** button scans every file currently loaded in the FITS Reviewer:

1. **Calibration frames are skipped** — the `IMAGETYP` FITS header is read first. Files containing `bias`, `dark`, or `flat` (case-insensitive) are left untouched.

2. **Star detection** runs on the raw pixel data of each light frame:
   - Background level and noise (sigma) are estimated from a sampled median and MAD.
   - Pixels above `background + 5σ` are thresholded into a binary mask.
   - Connected blobs are found with a 4-connected depth-first search.
   - Each blob is classified by its bounding-box size and aspect ratio:
     - **Valid star** — small, roughly round blob (aspect ratio < 4.5, bbox ≤ 50 px)
     - **Star trail** — highly elongated blob (aspect ratio ≥ 4.5)
     - **Out of focus** — large round blob (bbox > 50 px)

3. A frame is **flagged for deletion** if any of the following are true:
   - One or more star trails detected
   - Out-of-focus blobs detected
   - Fewer than **10 valid stars** detected

4. A **results dialog** lists every flagged file with its reason and star count.  
   Each file has a checkbox (checked by default). Uncheck any files you want to keep, then click **Delete Checked** to permanently remove only the checked files.

> Analysis runs in a background thread. The status bar shows `Auto-reviewing N/total…` while processing. Large FITS files (e.g. 6000×4000) take 1–3 seconds each.

---

## Install / uninstall

### System-wide install

```bash
sudo cmake --install build
# Binary installed as /usr/local/bin/astro-toolkit
```

### Run without installing

```bash
./build/astro_toolkit
```

### Windows

A `WIN32` GUI executable is produced automatically when building with MSVC or MinGW.  
Install Qt6, libcurl, cfitsio, and nlohmann-json via [vcpkg](https://vcpkg.io) or manually, then:

```cmd
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
