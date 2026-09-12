# cod1plus

**cod1plus.so** is the server-side half of [COD1.6X](https://github.com/cod1plus/client): a 32-bit
`LD_PRELOAD` module for the Call of Duty 1 (1.5) Linux dedicated server. It moves the server to the
COD1.6X network (protocol 10, new master, client version gate), makes the server test the same player
pose the clients draw (lean / swing / animation fixes), pushes fair-play cvar limits to every client,
and binds the server to an FPSChallenge match.

> Status: 1.6.5 - pairs with COD1.6X clients 1.6+. See [CHANGELOG.md](CHANGELOG.md).

## Installation

**Prerequisites:** a working Call of Duty 1.5 Linux dedicated server (`cod_lnxded`) with its 32-bit
libraries, including `libstdc++.so.5` (see `docker/Dockerfile` for a Debian recipe), glibc 2.34+ and `curl`.

1. Download **`cod1plus.so`** from the latest release:
   https://github.com/cod1plus/cod1plushookserver/releases/latest
2. Copy it next to `cod_lnxded`.
3. Launch the server with the module preloaded (this is the production line):
   ```sh
   exec env LD_PRELOAD=./cod1plus.so ./cod_lnxded +set dedicated 2 +set fs_homepath "$SERVER_DIR" \
     +set fs_game __rPAMv115b5 +set sv_punkbuster 0 +set net_ip 0.0.0.0 +set net_port 28960 \
     +set logfile 2 +set g_gametype sd +exec __autoexec.cfg +map mp_harbor
   ```
   The console must show `[cod1plus] Loaded (version x.y.z)` and `[cod1reloaded] protocol -> 10`.

**Optional**
- `competitive.cfg` next to `cod_lnxded` (start from [`competitive.cfg.example`](competitive.cfg.example)):
  cvar locks / ranges pushed to every client. Re-read live, no restart.
- **Master server:** `python3 master/cod1master.py` (UDP 20510); `master/cod1master.service` is the
  systemd unit. Point game servers at it with `COD1RELOADED_MASTER=<ip>`.
- **FPSChallenge match:** add `+match create <match_id>` to the launch line; the module fetches the match
  config into `matchdata.cfg` ([`matchdata.cfg.example`](matchdata.cfg.example) shows a hand-written one).
- **PAM mod:** the competitive mod the server runs (`fs_game __rPAMv115b5`) lives in its own repo:
  https://github.com/cod1plus/pam

## What's included

- Protocol 10, master repoint, client version / build gate (follows the client releases automatically).
- Lean / pose / swing sync and animation-index clamp: the server hits the pose the client draws, and
  "Player animation index out of range" no longer drops anyone.
- `competitive.cfg` -> `sv_competitive` systeminfo lock (the PunkBuster replacement) + cheat gate (log or kick).
- `g_useGear` forced to 0, 40-tick `snaps` cap (set `sv_fps 40` in the server cfg).
- Antilag (experimental, `g_antilag` cvar).

## Configuration

Everything is an environment variable on the launch line; the defaults are the production values.

| Variable | Default | Effect |
|---|---|---|
| `COD1RELOADED_MASTER` | `87.106.7.52` | master server to heartbeat to (`ip[:port]`, port 20510) |
| `COD1RELOADED_MIN_VERSION` | `16` | minimum client version (16 = 1.6) |
| `COD1RELOADED_ALLOW_UNVERSIONED` | `0` | `1` lets vanilla / unversioned clients connect |
| `COD1RELOADED_MIN_BUILD` | `0` (off) | manual minimum client build, e.g. `10602` |
| `COD1RELOADED_MIN_BUILD_AUTO` / `_GRACE_H` / `_POLL_MIN` | `1` / `0` / `5` | follow the client's latest release (needs curl); grace hours; poll minutes |
| `COD1RELOADED_SNAPS_CAP` | `40` | maximum `snaps` honoured |
| `COD1RELOADED_COMPETITIVE` / `COD1RELOADED_COMPETITIVE_FILE` | on / `competitive.cfg` | `0` disables the cvar push; alternate file path |
| `COD1RELOADED_CHEATGATE` | `log` | `0` off, `log` observe only, `kick` |
| `COD1RELOADED_GEAR` | unset | `1` leaves `g_useGear` to the server cfg |
| `COD1RELOADED_POSE_SYNC` / `_SWING_SYNC` / `_ANIM_CLAMP` | on | `0` disables the fix (debug only) |
| `COD1RELOADED_ANTILAG` | installed, inactive | `0` = do not install; `rcon g_antilag 1` activates it |
| `COD1PLUS_MODE` | `live` | `dev` skips the FPSChallenge match fetch |
| `COD1MASTER_PORT` / `COD1MASTER_PUBLIC_IP` / `COD1MASTER_TIMEOUT` | `20510` / unset / `300` | master server |

## Building from source

```sh
sudo apt-get install -y gcc-multilib     # 32-bit toolchain (WSL Ubuntu works)
sh scripts/build.sh                      # -> build/cod1plus.so
```

The script refuses to produce a module needing more than `GLIBC_2.34`: modern glibc (2.38+) silently
rebinds `strtol` / `sscanf` to `__isoc23_*` symbols the game host does not have, so every file is compiled
with `-include src/glibc_compat.h` - keep it. `src/shared/` is a byte-identical copy of the client repo's
file; change it in both places.

## Local test server

- **Docker:** `sh docker/run.sh [map]` builds an i386 Debian image with `libstdc++5` and runs your server
  folder (`COD1_SERVER_DIR`) with the freshly built module. Connect with `/connect 127.0.0.1:28965`.
- **WSL smoke test:** `bash scripts/smoketest.sh 30` copies a server folder (`COD1_SERVER_DIR`), boots
  `cod_lnxded` with the module for 30 s and greps every module's install line. Exit 124 = still running = OK.

## Release

1. Update `CHANGELOG.md`, commit, tag: `git tag 1.6.6 && git push --tags`
   (major.minor follows the COD1.6X client generation, patch is free).
2. On GitHub, **Releases -> Draft a new release** for that tag and publish it.
3. The **Release** workflow builds `cod1plus.so` with the tag compiled in and attaches `cod1plus.so`,
   `competitive.cfg.example`, `matchdata.cfg.example` and `SHA256SUMS` to the release
   (or run it by hand from the Actions tab for an existing tag).

## License

GPL-3.0 - see [LICENSE](LICENSE).
