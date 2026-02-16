# cod1plus

Competitive stats tracker for Call of Duty 1 (v1.5) Search & Destroy. Hooks into the dedicated server via `LD_PRELOAD`, captures round-end events from PAM mod, and forwards fpschallenge.eu-compatible payloads through a Node.js backend.

## Architecture

```
PAM sd.gsc (round end)
    |
    v
qconsole.log  -->  cod1plus.so (log tailer)  -->  Node.js backend  -->  fpschallenge.eu
                   parses [STATS_EVENT]           stores + forwards
                   builds JSON payload
```

## Project Structure

```
cod1plus/
  src/
    cod1plus.c              LD_PRELOAD shared library (C, 32-bit)
  backend/
    server.js               Express HTTP backend
    package.json
  scripts/
    build.sh                Compilation script
  build/
    cod1plus.so             Compiled library
  matchdata.cfg             Match configuration (auto-generated or manual)
  zzzzz_vcodpam_nolib_2_15/ PAM mod with stats tracking (GSC)
  archive/                  Legacy code
  test/                     Test scripts and mock data
```

## Quick Start

### 1. Build

Requires `gcc` with 32-bit support (`gcc-multilib` on Debian/Ubuntu).

```bash
bash scripts/build.sh
```

### 2. Start the Backend

```bash
cd backend && npm install
PORT=3005 node server.js
```

### 3. Run the Server

**With FPSChallenge (production):**

The server is started with `+match create {id}`. cod1plus.so detects the match ID, calls the backend which fetches match details from the FPSChallenge API, and auto-generates `matchdata.cfg`.

```bash
LD_PRELOAD=./cod1plus.so ./cod_lnxded \
  +exec server.cfg \
  +match create 204673 \
  +map mp_harbor
```

**With manual config (local dev/testing):**

Create a `matchdata.cfg` file (see format below) and start without `+match create`:

```bash
LD_PRELOAD=./cod1plus.so ./cod_lnxded \
  +exec server.cfg \
  +map mp_harbor
```

## Match Setup Flow (Production)

When launched with `+match create {id}`:

1. `cod1plus.so` constructor parses `/proc/self/cmdline` for the match ID
2. Calls `GET http://localhost:{port}/api/match_setup?id={id}` on the local backend
3. Backend calls `GET https://fpschallenge.eu/api/v2/{game}/match/{id}`
4. Backend maps the API response and writes `matchdata.cfg`
5. `cod1plus.so` reads `matchdata.cfg` and starts the log tailer
6. Match is ready

If the backend is not yet running, cod1plus.so retries up to 10 times (2s interval) before falling back to any existing `matchdata.cfg`.

## matchdata.cfg Format

```cfg
// Match metadata
set cod1plus_match_id       "204673"
set cod1plus_start_time     "2026-02-16T20:00:00.000Z"

// Team 1
set cod1plus_team1_id       "220379"
set cod1plus_team1_name     "Team Alpha"
set cod1plus_team1_tag      "ALPHA"
set cod1plus_team1_side     "1"

// Team 2
set cod1plus_team2_id       "220378"
set cod1plus_team2_name     "Team Bravo"
set cod1plus_team2_tag      "BRAVO"

// Match rules
set cod1plus_format         "BO1"
set cod1plus_mr             "MR12"
set cod1plus_half_round     "12"
set cod1plus_score_limit    "13"
set cod1plus_round_limit    "24"

// Endpoints
set cod1plus_api_url        "http://localhost:3005/api/round_end"
set cod1plus_demo_url       ""
set cod1plus_logfile        "./qconsole.log"

// Players: name,uuid,team (1 or 2)
set cod1plus_player1        "PlayerOne,a0e19aa5-80ca-492c-a005-6169ea032ac3,1"
set cod1plus_player2        "PlayerTwo,ec1a67c3-8245-42a7-a726-a4c22e7a7629,2"
```

`team1_side` defines which team starts as allies (1) or axis (2). After halftime, sides swap automatically.

## Backend API

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/match_setup?id={id}` | Fetch match config from FPSChallenge, write `matchdata.cfg` |
| POST | `/api/round_end` | Receive round-end payload from cod1plus.so, store and forward |
| GET | `/api/round_end` | Retrieve all stored round-end events |
| POST | `/api/stats` | Generic stats storage |
| GET | `/api/stats` | Retrieve stored stats |

## Environment Variables

### Backend (Node.js)

| Variable | Default | Description |
|----------|---------|-------------|
| `PORT` | `3000` | Backend listening port |
| `FPS_API_BASE` | `https://fpschallenge.eu/api/v2` | FPSChallenge API base URL |
| `FPS_GAME` | `cod1` | Game identifier for API path (`cod1`, `cod2`) |
| `FPS_API_URL` | (empty) | URL to forward round-end payloads to |
| `MATCHDATA_PATH` | `../matchdata.cfg` | Path to write generated match config |

### cod1plus.so (C)

| Variable | Default | Description |
|----------|---------|-------------|
| `COD1PLUS_BACKEND_PORT` | `3005` | Port of the local backend for match setup |

## Stats Event Format

PAM's `sd.gsc` prints a `[STATS_EVENT]` line to `qconsole.log` at the end of each round:

```
[STATS_EVENT]r=3,as=2,xs=1,rw=allies,ht=0,bp=1,ps=Player1:allies:2:1:0:120:1:1:0:2.5:1:30:40.00|Player2:axis:1:2:0:80:0:0:1:1.0:0:0:26.67
```

Fields: `r`=round, `as`=allies score, `xs`=axis score, `rw`=round winner, `ht`=halftime flag, `bp`=bomb planted.

Player fields: `name:team:kills:deaths:assists:damage:grenades:plants:defuses:score:headshots:grenade_damage:adr`

## Tested On

- CoD1 v1.5 Linux (`cod_lnxded`)
- Debian/Ubuntu (32-bit compilation)
- PAM mod (vcodpam_nolib 2.15)

## License

GPL-3.0
