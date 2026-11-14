<?php
/**
 * OrbitSDR API - caching proxy for satellite data and map tiles
 * ==============================================================
 *
 * OrbitSDR asks this script instead of the public services, so CelesTrak,
 * SatNOGS, NASA and EOX see one polite client (your server) no matter how
 * many OrbitSDR installations use it.
 *
 *   ?group=amateur            TLE group from CelesTrak (whitelisted names)
 *   ?catnr=25544,43017        TLEs by NORAD number (up to 50)
 *   ?name=ISS                 CelesTrak name search
 *   ?transmitters=25544       SatNOGS DB transmitter list (JSON)
 *   ?map=1                    world map picture (NASA Blue Marble)
 *   ?tile=osm/12/1234/4567    map tile layer/z/y/x (EOX Maps, WGS84 grid)
 *   ?status=1                 health check (JSON)
 *
 * Cache: TLEs and transmitter lists in MySQL / MariaDB or PostgreSQL (PDO), or in
 * plain files when no database is configured. The map picture and the tiles are
 * always files. When an upstream service fails, the last good copy is served
 * ("X-Cache: STALE") and the upstream is not asked again for 'retry_after' seconds.
 *
 * Setup
 *   1. Copy this file to your server, e.g. https://example.com/orbitsdr/orbitsdr-api.php
 *   2. Fill in $CONFIG below (everything is set in this file).
 *      Database tables are created automatically on the first request.
 *   3. In OrbitSDR, set the address in orbitsdr.ini:
 *          api_url = https://example.com/orbitsdr/orbitsdr-api.php
 *      The key (ORBITSDR_APP) is built into the app.
 *   4. Check https://example.com/orbitsdr/orbitsdr-api.php?status=1
 *
 * Requirements: PHP 7.4+ with curl (or allow_url_fopen), pdo_mysql / pdo_pgsql
 * only when a database is used.
 */

declare(strict_types=1);

// ---------------------------------------------------------------------------
//  Configuration
// ---------------------------------------------------------------------------
$CONFIG = [
    // 'mysql', 'pgsql' or 'file'
    'db_driver'    => 'file',
    'db_host'      => 'localhost',
    'db_port'      => '',            // empty = default port
    'db_name'      => 'orbitsdr',
    'db_user'      => 'orbitsdr',
    'db_pass'      => '',
    'table_prefix' => 'orbitsdr_',

    // Folder for the file cache, the map picture and the tiles (writable by PHP,
    // outside the web root if you can; an .htaccess denying access is created in it)
    'cache_dir'    => __DIR__ . '/orbitsdr-cache',

    // Key the app sends in the X-OrbitSDR-Key header. Public on purpose (OrbitSDR is
    // open source, the same key is built into the app): it only keeps random scanners
    // out. Abuse is handled by the cache and the rate limits below. Empty = no check.
    'api_key'      => 'ORBITSDR_APP',

    // Requests per client IP per hour (0 = no limit). Tiles have their own limit:
    // zooming around a city loads a few hundred of them.
    'rate_limit'      => 300,
    'tile_rate_limit' => 6000,

    // How long answers stay fresh (seconds). CelesTrak asks clients not to download
    // the same data more than once every 2 hours.
    'ttl_tle'      => 4 * 3600,
    'ttl_tx'       => 24 * 3600,
    'ttl_tile'     => 180 * 86400,   // the mosaics change once a year
    'retry_after'  => 30 * 60,       // after an upstream failure

    // Disk space for tiles (MB). Above it the oldest tiles are removed (checked now and then).
    'tile_cache_mb' => 4000,

    // Identify yourself to the upstream services (a contact helps if anything goes wrong)
    'user_agent'   => 'OrbitSDR-API/1.1 (caching proxy)',

    // Upstream fetches allowed per request for ?catnr (the rest comes with the next request)
    'max_upstream_per_request' => 15,
];

const API_VERSION = '1.1';

const CELESTRAK_GROUPS = [
    'amateur', 'weather', 'stations', 'cubesat', 'satnogs', 'noaa', 'goes', 'visual',
    'science', 'education', 'geo', 'gnss', 'active', 'starlink', 'oneweb', 'iridium-NEXT',
    'orbcomm', 'globalstar', 'resource', 'sarsat', 'dmc', 'tdrss', 'argos', 'planet', 'spire',
    'last-30-days', 'gps-ops', 'glo-ops', 'galileo', 'beidou', 'sbas', 'radar', 'engineering',
    'geodetic', 'musson', 'other-comm', 'x-comm', 'military',
];

const MAP_URL = 'https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-topography-bathymetry/december/world.topo.bathy.200412.3x5400x2700.jpg';

// EOX Maps tile layers (WGS84 grid): id => [extension, deepest level]
const TILE_LAYERS = [
    's2cloudless-2024' => ['jpg', 14],
    'osm'              => ['jpg', 16],
    'overlay_bright'   => ['png', 14],
];
const TILE_URL = 'https://tiles.maps.eox.at/wmts/1.0.0/%s/default/WGS84/%d/%d/%d.%s';

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
function fail(int $code, string $msg): void
{
    http_response_code($code);
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store');
    echo "OrbitSDR API: $msg\n";
    exit;
}

/** @return array{0:int,1:string,2:string} [http code, body, error] */
function upstreamGet(string $url, string $ua, int $timeout = 30): array
{
    if (function_exists('curl_init')) {
        $ch = curl_init($url);
        curl_setopt_array($ch, [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_FOLLOWLOCATION => true,
            CURLOPT_MAXREDIRS      => 3,
            CURLOPT_CONNECTTIMEOUT => 10,
            CURLOPT_TIMEOUT        => $timeout,
            CURLOPT_USERAGENT      => $ua,
            CURLOPT_ENCODING       => '',
        ]);
        $body = curl_exec($ch);
        $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
        $err  = curl_error($ch);
        curl_close($ch);
        return [$code, is_string($body) ? $body : '', $err];
    }
    $ctx = stream_context_create(['http' => [
        'timeout' => $timeout, 'user_agent' => $ua, 'ignore_errors' => true, 'follow_location' => 1,
    ]]);
    $body = @file_get_contents($url, false, $ctx);
    $code = 0;
    if (isset($http_response_header[0]) && preg_match('#\s(\d{3})\s#', $http_response_header[0], $m)) $code = (int)$m[1];
    return [$code, is_string($body) ? $body : '', $body === false ? 'request failed' : ''];
}

/** Keeps only well formed TLE sets (name line optional); '' when there are none. */
function cleanTle(string $raw): string
{
    $lines = preg_split('/\r?\n/', $raw);
    $out = [];
    $n = count($lines);
    for ($i = 0; $i + 1 < $n; $i++) {
        $a = rtrim($lines[$i]);
        $b = rtrim($lines[$i + 1]);
        if (strlen($a) >= 69 && $a[0] === '1' && $a[1] === ' ' && strlen($b) >= 69 && $b[0] === '2') {
            $name = $i > 0 ? trim($lines[$i - 1]) : '';
            if ($name !== '' && !(strlen($name) >= 69 && ($name[0] === '1' || $name[0] === '2'))) $out[] = $name;
            $out[] = $a;
            $out[] = $b;
            $i++;
        }
    }
    return $out ? implode("\n", $out) . "\n" : '';
}

// ---------------------------------------------------------------------------
//  Cache store (PDO or files) + rate limit
// ---------------------------------------------------------------------------
final class Store
{
    private ?PDO $db = null;
    private string $p;
    private string $dir;
    private string $driver = 'file';

    public function __construct(array $c)
    {
        $this->dir = rtrim($c['cache_dir'], '/\\');
        $this->p = preg_replace('/[^a-z0-9_]/i', '', $c['table_prefix']);
        if (!is_dir($this->dir) && !@mkdir($this->dir, 0775, true)) fail(500, 'cannot create cache_dir');
        if (!is_file($this->dir . '/.htaccess')) @file_put_contents($this->dir . '/.htaccess', "Require all denied\nDeny from all\n");
        if (in_array($c['db_driver'], ['mysql', 'pgsql'], true)) {
            try {
                $port = $c['db_port'] !== '' ? ';port=' . $c['db_port'] : '';
                $dsn = $c['db_driver'] === 'mysql'
                    ? "mysql:host={$c['db_host']}{$port};dbname={$c['db_name']};charset=utf8mb4"
                    : "pgsql:host={$c['db_host']}{$port};dbname={$c['db_name']}";
                $this->db = new PDO($dsn, $c['db_user'], $c['db_pass'], [
                    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_TIMEOUT => 5,
                ]);
                $this->driver = $c['db_driver'];
                $this->createTables();
            } catch (Throwable $e) {
                error_log('OrbitSDR API: database unavailable, using the file cache: ' . $e->getMessage());
                $this->db = null;
                $this->driver = 'file';
            }
        }
        if (!$this->db && !is_dir($this->dir . '/data')) @mkdir($this->dir . '/data', 0775, true);
    }

    public function driver(): string { return $this->driver; }
    public function dir(): string { return $this->dir; }

    private function createTables(): void
    {
        $p = $this->p;
        $text = $this->driver === 'mysql' ? 'MEDIUMTEXT' : 'TEXT';
        $this->db->exec("CREATE TABLE IF NOT EXISTS {$p}cache (
            cache_key    VARCHAR(191) NOT NULL PRIMARY KEY,
            body         $text NOT NULL,
            content_type VARCHAR(64) NOT NULL,
            fetched_at   BIGINT NOT NULL,
            attempted_at BIGINT NOT NULL)");
        $this->db->exec("CREATE TABLE IF NOT EXISTS {$p}ratelimit (
            bucket  VARCHAR(80) NOT NULL PRIMARY KEY,
            hits    INT NOT NULL,
            expires BIGINT NOT NULL)");
    }

    private function file(string $key): string { return $this->dir . '/data/' . sha1($key) . '.cache'; }

    /** @return array{body:string,type:string,fetched:int,attempted:int}|null */
    public function get(string $key): ?array
    {
        if ($this->db) {
            $st = $this->db->prepare("SELECT body, content_type, fetched_at, attempted_at FROM {$this->p}cache WHERE cache_key = ?");
            $st->execute([$key]);
            $r = $st->fetch(PDO::FETCH_NUM);
            return $r ? ['body' => (string)$r[0], 'type' => (string)$r[1], 'fetched' => (int)$r[2], 'attempted' => (int)$r[3]] : null;
        }
        $f = $this->file($key);
        if (!is_file($f)) return null;
        $v = @unserialize((string)file_get_contents($f), ['allowed_classes' => false]);
        return is_array($v) ? $v : null;
    }

    public function put(string $key, string $body, string $type, int $fetched, int $attempted): void
    {
        if ($this->db) {
            $p = $this->p;
            $sql = $this->driver === 'mysql'
                ? "INSERT INTO {$p}cache (cache_key, body, content_type, fetched_at, attempted_at) VALUES (?, ?, ?, ?, ?)
                   ON DUPLICATE KEY UPDATE body = VALUES(body), content_type = VALUES(content_type),
                   fetched_at = VALUES(fetched_at), attempted_at = VALUES(attempted_at)"
                : "INSERT INTO {$p}cache (cache_key, body, content_type, fetched_at, attempted_at) VALUES (?, ?, ?, ?, ?)
                   ON CONFLICT (cache_key) DO UPDATE SET body = EXCLUDED.body, content_type = EXCLUDED.content_type,
                   fetched_at = EXCLUDED.fetched_at, attempted_at = EXCLUDED.attempted_at";
            $this->db->prepare($sql)->execute([$key, $body, $type, $fetched, $attempted]);
            return;
        }
        atomicWrite($this->file($key), serialize(['body' => $body, 'type' => $type, 'fetched' => $fetched, 'attempted' => $attempted]));
    }

    public function count(): int
    {
        if ($this->db) return (int)$this->db->query("SELECT COUNT(*) FROM {$this->p}cache")->fetchColumn();
        return count(glob($this->dir . '/data/*.cache') ?: []);
    }

    /** One more request in this hour's bucket; false when over the limit. */
    public function rateHit(string $bucket, int $limit): bool
    {
        if ($limit <= 0) return true;
        $now = time();
        $expires = $now - ($now % 3600) + 3600;
        if ($this->db) {
            $p = $this->p;
            if (mt_rand(0, 200) === 0) $this->db->prepare("DELETE FROM {$p}ratelimit WHERE expires < ?")->execute([$now]);
            $sql = $this->driver === 'mysql'
                ? "INSERT INTO {$p}ratelimit (bucket, hits, expires) VALUES (?, 1, ?) ON DUPLICATE KEY UPDATE hits = hits + 1"
                : "INSERT INTO {$p}ratelimit (bucket, hits, expires) VALUES (?, 1, ?) ON CONFLICT (bucket) DO UPDATE SET hits = {$p}ratelimit.hits + 1";
            $this->db->prepare($sql)->execute([$bucket, $expires]);
            $st = $this->db->prepare("SELECT hits FROM {$p}ratelimit WHERE bucket = ?");
            $st->execute([$bucket]);
            return (int)$st->fetchColumn() <= $limit;
        }
        $d = $this->dir . '/rate';
        if (!is_dir($d)) @mkdir($d, 0775, true);
        if (mt_rand(0, 200) === 0) foreach (glob($d . '/*') ?: [] as $f) if (filemtime($f) < $now - 7200) @unlink($f);
        $fh = @fopen($d . '/' . sha1($bucket), 'c+');
        if (!$fh) return true;
        flock($fh, LOCK_EX);
        $hits = (int)stream_get_contents($fh) + 1;
        ftruncate($fh, 0);
        rewind($fh);
        fwrite($fh, (string)$hits);
        flock($fh, LOCK_UN);
        fclose($fh);
        return $hits <= $limit;
    }
}

function atomicWrite(string $f, string $data): bool
{
    $dir = dirname($f);
    if (!is_dir($dir)) @mkdir($dir, 0775, true);
    $tmp = $f . '.' . getmypid() . '.tmp';
    if (@file_put_contents($tmp, $data, LOCK_EX) === false) return false;
    return @rename($tmp, $f);
}

/**
 * Cached upstream request (text answers).
 * Returns [body, cacheState] or null when there is nothing to serve.
 */
function cached(Store $s, array $c, string $key, string $url, int $ttl, string $type, callable $validate, bool $allowUpstream = true): ?array
{
    $now = time();
    $e = $s->get($key);
    $have = $e && $e['body'] !== '';   // an entry with an empty body only remembers a failure
    if ($have && $now - $e['fetched'] < $ttl) return [$e['body'], 'HIT'];
    if (!$allowUpstream || ($e && $now - $e['attempted'] < $c['retry_after'] && $e['attempted'] > $e['fetched'])) {
        return $have ? [$e['body'], 'STALE'] : null;
    }
    [$code, $raw] = upstreamGet($url, $c['user_agent']);
    // CelesTrak: "GP data has not updated since your last successful download" = ours is current
    if ($have && strpos($raw, 'has not updated since') !== false) {
        $s->put($key, $e['body'], $e['type'], $now, $now);
        return [$e['body'], 'HIT'];
    }
    $body = ($code >= 200 && $code < 300) ? $validate($raw) : '';
    if ($body !== '') {
        $s->put($key, $body, $type, $now, $now);
        return [$body, 'MISS'];
    }
    if ($have) {
        $s->put($key, $e['body'], $e['type'], $e['fetched'], $now);
        return [$e['body'], 'STALE'];
    }
    $s->put($key, '', $type, 0, $now);   // remember the failure (retry_after)
    return null;
}

function send(string $body, string $type, string $state, int $maxAge): void
{
    header('Content-Type: ' . $type);
    header('X-Cache: ' . $state);
    header('Cache-Control: public, max-age=' . $maxAge);
    header('Content-Length: ' . strlen($body));
    echo $body;
    exit;
}

function sendFile(string $f, string $type, string $state, int $maxAge): void
{
    header('Content-Type: ' . $type);
    header('X-Cache: ' . $state);
    header('Cache-Control: public, max-age=' . $maxAge);
    header('Content-Length: ' . filesize($f));
    readfile($f);
    exit;
}

// ---------------------------------------------------------------------------
//  Request
// ---------------------------------------------------------------------------
header('Access-Control-Allow-Origin: *');
$store = new Store($CONFIG);

if (isset($_GET['status'])) {
    header('Content-Type: application/json');
    echo json_encode([
        'ok' => true, 'version' => API_VERSION, 'cache' => $store->driver(), 'entries' => $store->count(),
        'map_cached' => is_file($store->dir() . '/worldmap.jpg'), 'tile_layers' => array_keys(TILE_LAYERS),
    ]);
    exit;
}

if ($CONFIG['api_key'] !== '') {
    $given = $_SERVER['HTTP_X_ORBITSDR_KEY'] ?? ($_GET['key'] ?? '');
    if (!is_string($given) || !hash_equals($CONFIG['api_key'], $given)) fail(401, 'missing or wrong API key');
}

$ipHash = substr(sha1(($_SERVER['REMOTE_ADDR'] ?? '') . '|' . __FILE__), 0, 20);   // no plain IPs stored
$hour = (int)(time() / 3600);
$isTile = isset($_GET['tile']);
if (!$store->rateHit(($isTile ? 't:' : 'r:') . $ipHash . ':' . $hour, $isTile ? $CONFIG['tile_rate_limit'] : $CONFIG['rate_limit'])) {
    header('Retry-After: 600');
    fail(429, 'too many requests, try again later');
}

$tleValidate = fn(string $raw): string => cleanTle($raw);

// --- TLE group ---
if (isset($_GET['group'])) {
    $g = (string)$_GET['group'];
    if (!in_array($g, CELESTRAK_GROUPS, true)) fail(400, 'unknown group');
    $url = 'https://celestrak.org/NORAD/elements/gp.php?GROUP=' . rawurlencode($g) . '&FORMAT=tle';
    $res = cached($store, $CONFIG, "group:$g", $url, $CONFIG['ttl_tle'], 'text/plain', $tleValidate);
    if (!$res) fail(502, 'CelesTrak is not reachable and nothing is cached yet');
    send($res[0], 'text/plain; charset=utf-8', $res[1], 3600);
}

// --- TLEs by catalog number ---
if (isset($_GET['catnr'])) {
    $ids = array_values(array_unique(array_filter(array_map('intval', explode(',', (string)$_GET['catnr'])), fn($v) => $v > 0 && $v < 1000000)));
    if (!$ids || count($ids) > 50) fail(400, 'catnr: 1 to 50 numbers');
    $out = '';
    $upstream = 0;
    $state = 'HIT';
    foreach ($ids as $id) {
        $url = 'https://celestrak.org/NORAD/elements/gp.php?CATNR=' . $id . '&FORMAT=tle';
        $allow = $upstream < $CONFIG['max_upstream_per_request'];
        $before = $store->get("catnr:$id");
        $res = cached($store, $CONFIG, "catnr:$id", $url, $CONFIG['ttl_tle'], 'text/plain', $tleValidate, $allow);
        if (!$before || time() - $before['fetched'] >= $CONFIG['ttl_tle']) $upstream++;
        if ($res) { $out .= $res[0]; if ($res[1] !== 'HIT') $state = $res[1]; }
    }
    if ($out === '') fail(404, 'nothing found');
    send($out, 'text/plain; charset=utf-8', $state, 3600);
}

// --- name search ---
if (isset($_GET['name'])) {
    $q = trim((string)$_GET['name']);
    if (strlen($q) < 2 || strlen($q) > 40) fail(400, 'name: 2 to 40 characters');
    $q = strtoupper($q);
    $url = 'https://celestrak.org/NORAD/elements/gp.php?NAME=' . rawurlencode($q) . '&FORMAT=tle';
    $res = cached($store, $CONFIG, 'name:' . $q, $url, $CONFIG['ttl_tle'], 'text/plain', $tleValidate);
    if (!$res) fail(404, 'nothing found');
    send($res[0], 'text/plain; charset=utf-8', $res[1], 3600);
}

// --- SatNOGS transmitters ---
if (isset($_GET['transmitters'])) {
    $id = (int)$_GET['transmitters'];
    if ($id <= 0 || $id >= 1000000) fail(400, 'transmitters: NORAD number');
    $url = 'https://db.satnogs.org/api/transmitters/?format=json&satellite__norad_cat_id=' . $id;
    $validate = function (string $raw): string {
        $j = json_decode($raw, true);
        if (!is_array($j) || ($j && !isset($j[0]))) return '';
        // keep only the fields OrbitSDR uses (smaller cache, stable format)
        $keep = ['description', 'mode', 'type', 'downlink_low', 'downlink_high', 'baud', 'alive', 'status'];
        $out = [];
        foreach ($j as $t) if (is_array($t)) $out[] = array_intersect_key($t, array_flip($keep));
        return (string)json_encode($out);
    };
    $res = cached($store, $CONFIG, "tx:$id", $url, $CONFIG['ttl_tx'], 'application/json', $validate);
    if (!$res) fail(502, 'SatNOGS is not reachable and nothing is cached yet');
    send($res[0], 'application/json', $res[1], 6 * 3600);
}

// --- world map picture (downloaded once, then served from disk) ---
if (isset($_GET['map'])) {
    $f = $store->dir() . '/worldmap.jpg';
    if (!is_file($f)) {
        $lock = $store->dir() . '/worldmap.lock';
        if (is_file($lock) && time() - filemtime($lock) < $CONFIG['retry_after']) fail(503, 'map download in progress or failed recently');
        @touch($lock);
        [$code, $body] = upstreamGet(MAP_URL, $CONFIG['user_agent'], 120);
        if ($code < 200 || $code >= 300 || strlen($body) < 100000 || substr($body, 0, 2) !== "\xFF\xD8") fail(502, 'could not download the map');
        atomicWrite($f, $body);
        @unlink($lock);
        sendFile($f, 'image/jpeg', 'MISS', 30 * 86400);
    }
    sendFile($f, 'image/jpeg', 'HIT', 30 * 86400);
}

// --- map tiles (EOX Maps, WGS84 grid): ?tile=layer/z/y/x ---
if ($isTile) {
    if (!preg_match('#^([a-z0-9_\-]+)/(\d{1,2})/(\d{1,6})/(\d{1,6})$#', (string)$_GET['tile'], $m)) fail(400, 'tile: layer/z/y/x');
    [, $layer, $z, $y, $x] = $m;
    $z = (int)$z; $y = (int)$y; $x = (int)$x;
    if (!isset(TILE_LAYERS[$layer])) fail(400, 'unknown tile layer');
    [$ext, $maxZ] = TILE_LAYERS[$layer];
    if ($z > $maxZ || $y >= (1 << $z) || $x >= (2 << $z)) fail(404, 'no such tile');
    $type = $ext === 'png' ? 'image/png' : 'image/jpeg';
    $f = $store->dir() . "/tiles/$layer/$z/$y/$x.$ext";
    $missing = $f . '.404';
    $now = time();
    if (is_file($f) && $now - filemtime($f) < $CONFIG['ttl_tile']) sendFile($f, $type, 'HIT', 30 * 86400);
    if (is_file($missing) && $now - filemtime($missing) < $CONFIG['ttl_tile']) fail(404, 'no such tile');
    $failMark = $f . '.fail';
    if (is_file($failMark) && $now - filemtime($failMark) < 120) {   // upstream trouble a moment ago
        if (is_file($f)) sendFile($f, $type, 'STALE', 3600);
        fail(502, 'tile server not reachable');
    }
    [$code, $body] = upstreamGet(sprintf(TILE_URL, $layer, $z, $y, $x, $ext), $CONFIG['user_agent'], 20);
    $isImage = strlen($body) > 8 && (substr($body, 0, 2) === "\xFF\xD8" || substr($body, 0, 4) === "\x89PNG");
    if ($code === 200 && $isImage) {
        atomicWrite($f, $body);
        @unlink($failMark);
        if (mt_rand(0, 500) === 0) trimTiles($store->dir() . '/tiles', (int)$CONFIG['tile_cache_mb']);
        send($body, $type, 'MISS', 30 * 86400);
    }
    if ($code === 404) { atomicWrite($missing, ''); fail(404, 'no such tile'); }
    atomicWrite($failMark, '');
    if (is_file($f)) sendFile($f, $type, 'STALE', 3600);
    fail(502, 'tile server not reachable');
}

/** Removes the least recently written tiles when the folder is over the limit. */
function trimTiles(string $dir, int $maxMb): void
{
    if ($maxMb <= 0 || !is_dir($dir)) return;
    $files = [];
    $total = 0;
    $it = new RecursiveIteratorIterator(new RecursiveDirectoryIterator($dir, FilesystemIterator::SKIP_DOTS));
    foreach ($it as $f) {
        if (!$f->isFile()) continue;
        $files[] = [$f->getMTime(), $f->getPathname(), $f->getSize()];
        $total += $f->getSize();
    }
    $limit = $maxMb * 1024 * 1024;
    if ($total <= $limit) return;
    sort($files);
    foreach ($files as [$t, $p, $sz]) {
        if ($total <= $limit * 0.9) break;
        if (@unlink($p)) $total -= $sz;
    }
}

fail(400, 'unknown request (see ?status=1)');
