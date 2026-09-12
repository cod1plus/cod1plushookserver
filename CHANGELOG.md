# Changelog

## 1.6.5 (unreleased)

Repository clean-up: the PAM mod moved to its own repo (https://github.com/cod1plus/pam), the legacy Node
backend, build outputs and personal files were dropped, `hooks.c` moved into `src/`, the module now prints
its version (`[cod1plus] Loaded (version x.y.z)`), CI builds on every push and releases attach the assets.

- swing_sync: the server swings legs / torso / lean exactly as the client draws them.
- pose_sync / anim_clamp: re-installed after every game-module reload (they went dead on the 2nd map).
- gear_force: `g_useGear 0` before every `G_InitGame` and every frame after.
- competitive.cfg: 40-tick rules (`snaps 40`, `cl_maxpackets 60 250`, `rate 30000`, `cl_timenudge -20 0`,
  `cl_packetdup 1 5`), input, rendering and audio locks; re-read live.

## 1.6.3

- Leaning-player hitbox: retuned the shared BG lean constant (server 2.5 -> 7.5) instead of mirroring the client.
- Stats: identity is the login uuid, never the in-game name; roster matching by normalized name as a fallback.
- Auto version gate follows the client releases; competitive cvar spec pushed as `sv_competitive`.
- Command line: quoted values and single-argv launch lines (FPSChallenge manager) parsed correctly.
- Overtime handling: `OverTime;` / `MatchEnd;` drive the match state, score limit raised by 4 per OT.

## 1.6.0

- Protocol 10, master repoint, client version gate, cheat gate, animation-index clamp.
- FPSChallenge match binding (`+match create <id>`) and round-stats transport.
