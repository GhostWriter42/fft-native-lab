# What already exists around FFT (researched 2026-09-29)

Short, sourced notes to inform the HD / multiplayer decision. Nothing here is a recommendation to redistribute game content.

* **An official HD remaster exists.** *Final Fantasy Tactics – The Ivalice Chronicles* (released 2025-09-30 on PS5/PS4, Xbox,
  Switch/Switch 2 and PC) has a "classic" mode (essentially a port of the original with the War of the Lions script) and an
  "enhanced" mode with remastered graphics, new script and full voice acting; QoL includes battle/cutscene speed-up and a
  top-down camera. Reports say Square Enix had to rebuild it after losing the original source code — which makes a
  community matching decomp all the more notable.
  ([Game Informer](https://gameinformer.com/state-of-play/2025/06/04/final-fantasy-tactics-the-ivalice-chronicles-is-a-remaster-that-includes),
  [GamesRadar](https://www.gamesradar.com/games/final-fantasy/45-minutes-of-final-fantasy-tactics-the-ivalice-chronicles-has-me-certain-that-square-enix-rebuilding-the-remaster-after-losing-the-original-source-code-was-worth-the-effort/),
  [Steam](https://store.steampowered.com/app/1004640/FINAL_FANTASY_TACTICS__The_Ivalice_Chronicles/))
  *Implication:* an HD overhaul of the PS1 game is not the only HD option any more; the differentiators of a PS1-based
  project are moddability, the original balance/quirks, and the multiplayer/netcode work.
* **Multiplayer prior art is official, and it is co-op/PvP.** *War of the Lions* (PSP) has two 2-player wireless modes:
  *Melee* (PvP) and *Rendezvous* (co-op missions). No online multiplayer romhack of the PS1 game turned up in a first
  search. *Implication:* co-op / PvP battles are the natural scope; an MMO is a different product.
  ([Final Fantasy Wiki](https://finalfantasy.fandom.com/wiki/Multiplayer))
* **Emulator route to HD needs no code port.** DuckStation supports texture replacement (used for other PS1 games such as
  Vagrant Story); upscaling and geometry-precision options exist in modern PS1 emulators. A PS1 HD *texture* pack for FFT
  itself was not found (the visible FFT packs target the PSP War of the Lions via PPSSPP).
  ([Emulation General Wiki: texture packs](https://emulation.gametechwiki.com/index.php/Texture_packs),
  [GBAtemp: DuckStation texture packs](https://gbatemp.net/threads/ps1-duckstation-texture-packs-are-now-a-thing-vagrant-story.662541/),
  [PPSSPP WotL pack](https://github.com/Zodi-ark/Final-Fantasy-Tactics-The-War-of-the-Lions-Texture-Pack))
* **Widescreen.** A "Widescreen codes (and patch)" thread exists on FFHacktics
  ([topic 12958](https://ffhacktics.com/smf/index.php?topic=12958.0)). Emulator-side, the generic "render 3D in display
  aspect ratio" hack is reported to cause errors in FFT's 3D maps while native settings do not
  ([NGEmu thread](https://www.ngemu.com/threads/final-fantasy-tactics-video-problem.208771/)) — so a proper widescreen
  needs game-side changes (camera/culling), which is what a decomp allows.
* **Sprite / animation tooling already exists** and should be reused or matched rather than reinvented: Shishi Sprite Editor,
  FFTSpriteEditorG (shows assembled frames while editing), FFT Animation Editor (FFTae), FFTPatcher Suite
  ([Tools](https://ffhacktics.com/wiki/Tools), [Shishi Sprite Editor](https://ffhacktics.com/wiki/Shishi_Sprite_Editor),
  [Spriting 101](https://ffhacktics.com/smf/index.php?topic=126.0),
  [FFTPatcher Suite](https://sb.ffhacktics.com/wiki/FFTPatcher_Suite)). Formation sprites live in `UNIT.BIN`, portraits in
  `WLDFACE.BIN` (per the same wiki); ripped sheets are on [The Spriters Resource](https://www.spriters-resource.com/playstation/fft/).
  A Sprite Modding Toolkit also exists for the 2025 remaster
  ([Nexus](https://www.nexusmods.com/finalfantasytacticstheivalicechronicles/mods/20?tab=docs)).
* **Community & tooling.** The FFHacktics forum and wiki are the hub for FFT romhacking, and the decomp credits them.
  ([FFHacktics](https://ffhacktics.com/smf/index.php?board=53.0))
* **Legal shape.** The decomp repo ships no game data; a bring-your-own-disc engine/patcher is the accepted model.
  Hosting a public multiplayer service with Square Enix's content is riskier than distributing a patch. (Not legal advice.)

Open questions for you: emulator-first HD (fast, no port) versus native port (slow, full control, needed for netplay you
own end to end); whether the goal is "the best PS1 FFT" or "FFT with online co-op".
