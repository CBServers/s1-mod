# S1x/s1-mod: A Modded Client

This is a client modification for S1!  
Developed by X Labs and [AlterWare](https://alterware.dev).  
This is a fork of S1x/[s1-mod](https://git.alterware.dev/alterware/s1-mod) with added CB patches.  
Big thanks to all the contributors.              
Join us on [Discord](https://cbservers.xyz/discord) for support.  
Follow the original project on [GitHub](https://github.com/alterware) or [Twitter](https://x.com/alterwaredev).

NOTE: This fork is not affiliated or endorsed by AlterWare. Please do not bug original client maintainers with support requests in regards to this fork.

## Changes
- Remove branding by default (`ui_showBranding`)
- Add master server IP/Port dvar (`masterServerIP` & `masterServerPort`)
- Add custom texture support with "images" folder (from OG S1x)
- Send in-game chat as "say/say_team" GSC notifies
- Fix bug with say commands not always registering
- Add GSC methods for IO
- Fix killcam slowdown (thanks to Joelrau h1 fix)
- Fix in-game scoreboard not showing sometimes
- Fix connection attempt if server has password and password is not set
- Use shared CB GUID keys

## Build
- Install [Visual Studio 2022][vs-link] and enable `Desktop development with C++`
- Install [Premake5][premake5-link] and add it to your system PATH
- Clone this repository using [Git][git-link]
- Update the submodules using ``git submodule update --init --recursive``
- Run Premake with the option ``premake5 vs2022`` (Visual Studio 2022). No other build systems are supported.
- Build project via solution file in `build\s1-mod.sln`.

Only the x64 platform is supported. Do not attempt to build for Windows ARM 64 or Win32.

### Premake arguments

| Argument                    | Description                                    |
|:----------------------------|:-----------------------------------------------|
| `--copy-to=PATH`            | Optional, copy the EXE to a custom folder after build, define the path here if wanted. |
| `--dev-build`               | Enable development builds of the client. |

### Experimental Campaign SP fastfile probe

Singleplayer builds register a manual `loadzone <fastfile_basename>` diagnostic command. It is disabled by default; enable it explicitly with `set sp_testLoadZoneEnabled 1`. The command accepts only a 1-64 character ASCII basename containing letters, digits, `_` or `-`, and only runs in Singleplayer after the asset database reports ready.

This is an experimental test hook, not a supported mod loader. It calls `DB_LoadXAssets` with `DB_ZONE_COMMON`, `freeFlags = 0`, and `DB_LOAD_SYNC`, following an upstream source precedent. Before testing, put the candidate `<fastfile_basename>.ff` in `sp_test_zones/` under the process working directory in a disposable staging tree. The hook tries the engine's normal lookup first, then falls back to that exact staged file only for the active test load; all other file requests pass through unchanged. No files are copied or installed by this change.

The SP flags, engine lookup behavior, and zone lifetime across map transitions still require runtime validation. This command does not auto-load from GSC or enable the weapon shop's `scr_sp_campaign_weapon_shop_assets_ready` gate. Do not treat the Campaign purchase flow as verified.

## Disclaimer

This software has been created purely for the purposes of
academic research. It is not intended to be used to attack
other systems. Project maintainers are not responsible or
liable for misuse of the software. Use responsibly.

[premake5-link]:          https://premake.github.io
[git-link]:               https://git-scm.com
[vs-link]:                https://visualstudio.microsoft.com/vs
[cla-link]:               https://alterware.dev/cla
