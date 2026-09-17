# Changelog

## 1.6.6 (unreleased)

- Ruleset (the cod2x way): the server publishes `sv_competitive_ruleset` from `ruleset.txt` (re-read live;
  else env `COD1RELOADED_RULESET`, default `codbase-2023-05`); 1.6.6+ clients download the full CoDBase
  PunkBuster list from https://github.com/cod1plus/rulesets and enforce it locally, `competitive.cfg` keeps
  priority for the cvars it names. `<id>@<version>` makes connected clients re-download a bumped list
  (hot reload of the whole list); `0` publishes nothing.
- cheat_gate reads the client's ruleset report (`cod1x_rs`): persistent violations are kickable
  (`COD1RELOADED_CHEATGATE=kick`), probe hits and pre-1.6.6 clients are logged only.
- cheat_gate now actually drops: the userinfo sweep queues, the game thread calls `SV_DropClient`
  (the 1.6.5 kick mode only logged "-> DROP").
- `COD1RELOADED_COMPETITIVE=0` keeps the G_RunFrame hook (gear force + drop queue), only the publication is empty.

## 1.6.5 (unreleased)

Repository clean-up: the PAM mod moved to its own repo (https://github.com/cod1plus/cod1pluspam), the legacy Node
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
