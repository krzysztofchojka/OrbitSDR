# OrbitSDR API (caching proxy)

`orbitsdr-api.php` sits between OrbitSDR and the public data sources:

| Request | Upstream | Cached for |
|---|---|---|
| `?group=amateur` | CelesTrak GP data (TLE), whitelisted group names | 4 h |
| `?catnr=25544,43017` | CelesTrak, up to 50 numbers per request | 4 h each |
| `?name=ISS` | CelesTrak name search | 4 h |
| `?transmitters=25544` | SatNOGS DB transmitters (trimmed JSON) | 24 h |
| `?map=1` | NASA Blue Marble world map (one file on disk) | forever |
| `?tile=layer/z/y/x` | EOX Maps tiles, WGS84 grid: `s2cloudless-2024` (Sentinel-2 photos), `osm` (street map), `overlay_bright` (names / borders) | 180 days |
| `?status=1` | health check | - |

Each OrbitSDR installation also caches everything locally, so the proxy sees few
requests. If an upstream service is down, the last good copy is served
(`X-Cache: STALE`) and the upstream is not asked again for 30 minutes (2 minutes for tiles).

## Installation

1. Upload `orbitsdr-api.php` to your web server.
2. Edit the `$CONFIG` block at the top. Everything is set in the script itself:
   - `db_driver`: `mysql` (MySQL / MariaDB), `pgsql` (PostgreSQL) or `file`. The tables
     (`orbitsdr_cache`, `orbitsdr_ratelimit`) are created automatically.
   - `cache_dir`: writable folder for the file cache, the map picture and the tiles. Put it
     outside the web root if you can. An `.htaccess` that denies access is created inside it.
   - `api_key`: `ORBITSDR_APP`, the key built into OrbitSDR. It is public on purpose
     (OrbitSDR is open source) and only keeps random scanners out. The cache and the rate
     limits are what protect the upstream services. Empty = no key check.
   - `rate_limit` / `tile_rate_limit`: requests per client IP per hour.
   - `tile_cache_mb`: disk space for tiles; the oldest are removed above it.
   - `user_agent`: add a contact so the upstream services can reach you if something goes wrong.
3. Open `https://your.server/path/orbitsdr-api.php?status=1`. You should see `"ok": true`.
4. In OrbitSDR, set the address in `orbitsdr.ini` (next to the program, or a copy in the
   settings folder: `%APPDATA%\OrbitSDR` on Windows, `~/.config/orbitsdr` elsewhere):

   ```ini
   api_url = https://your.server/path/orbitsdr-api.php
   ```

Requirements: PHP 7.4 or newer with the `curl` extension (or `allow_url_fopen`), and
`pdo_mysql` / `pdo_pgsql` only when a database is used.

Client IP addresses are never stored, only salted hashes for the hourly rate limit.

## Tile licences

The tiles come from [EOX Maps](https://maps.eox.at). OrbitSDR shows the attribution on the map.
- Sentinel-2 cloudless 2024: CC BY-NC-SA 4.0 (free for non-commercial use, with attribution).
- OpenStreetMap layers: © OpenStreetMap contributors, rendering © EOX.
