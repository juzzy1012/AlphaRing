# Native Roster Investigation (Track B)

Goal: make AlphaRing's local co-op players appear in **MCC's own native roster UI
using the game's assets**, not an ImGui clone. Source: UE SDK dump at
`C:\Dumper-7\4.21.1-1003+__depot_MCC-MCC` (UE 4.21.1, MCC 1.3528 Steam).

## What the native roster actually is

It's a **UMG / MVVM** widget stack, not Scaleform/.gfx:

- Widgets (Blueprint, real game assets): `WBP_Roster`, `WBP_MCCRosterButton1/2`,
  `WBP_Nameplate`, `WBP_RosterMatchmakingState`, `WBP_RosterSettingOption`.
- Native classes: `MCC.MCCRosterWidget`, `MCC.MCCRosterViewModel`,
  `MCC.MCCLayerManager` (owns `RosterViewModel` + `RosterAvailabilityTable`),
  `MCC.MCCNameplateOverlayWidget`.
- State/visibility: `EMCCRosterState`, `MCCRosterAvailabilityData`
  (`IsRosterAvailable`, `ScreenName`, `ScreenID`), `MCCMainMenuScreen.OnRosterStateChange`.

## The data the roster renders (key finding)

The roster is bound to a **session player list**:

- `MCCRosterViewModel.GetGameSessionViewModel()` → `MCC.MCCGameSessionViewModel`.
- `MCCGameSessionViewModel.Players` — **ArrayProperty of `MCCSessionPlayerViewModel*`**.
  This array is what the roster widget iterates to draw player cards.
- `MCCGameSessionViewModel.LocalSlotItemViewModel`, `PlayerCountLabelText`.
- Each `MCCSessionPlayerViewModel` carries the full native presentation:
  `NameplateTextureNameBindable`, `Emblem1/2Texture`, `RankTexture`,
  `PreferredInputDeviceStateIconTexture`, `ClanTag`, `XPProgression`,
  `NameplateColourBindable`, `PlayerStateIconVisibility`, plus a
  `SessionPlayerDelegate` for change notification.

So a "native player card" = one `MCCSessionPlayerViewModel` added to
`MCCGameSessionViewModel.Players`, with the bindables populated; the existing
`WBP_*` widgets then render it with real MCC assets automatically.

## The open question (needs one live instrumentation run)

Is `MCCGameSessionViewModel.Players` populated from **local `ULocalPlayer`s**
(which our splitscreen backend already makes MCC create — viewports work today),
or from the **online session/party model** (Xbox Live / EOS members)?

- If local-player driven → our players may already be eligible; we likely just
  need to trigger roster availability / a refresh for the campaign-splitscreen
  screen. **(Best case.)**
- If session-model driven → we must construct `MCCSessionPlayerViewModel`
  objects for our locals and inject them into `Players` + fire the delegate, or
  spoof session membership. **(Hard case.)**

No `MCC.*` function named for couch/guest/local-player *join* exists in the dump
(`SplitScreen|Couch|Guest|AddLocalPlayer|RegisterLocalPlayer` → none), which
hints the local-join path is lower-level (engine LocalPlayer + session
subsystem) rather than a single MCC entry point.

## Proposed next experiment (single game run)

Instrument, don't guess. With the UE object primitives (already written on the
dev branch, portable here), at runtime:

1. Find the live `MCCGameSessionViewModel`; read `Players.Num` on the menu and
   while in splitscreen, logging each `MCCSessionPlayerViewModel`'s name/state.
2. Hook/observe `MCCRosterViewModel.OnScreenUpdated` and
   `MCCMainMenuScreen.OnRosterStateChange` to learn when/why the roster shows.
3. Determine whether our backend's extra local players ever reach `Players`.

Outcome decides the implementation path (refresh-only vs ViewModel injection vs
asset-reuse overlay).

## Asset inventory (for the game-asset look regardless of path)

- Texture packs: `Data/UI/TexturePacks/ControllerTexturePack.perm.bin`,
  `GlobalUITexturePack`, `MainMenuTexturePack` (referenced by
  `Data/UI/texturepacks.xml`).
- Nameplate/emblem/rank textures are referenced by name on
  `MCCSessionPlayerViewModel` — reachable as UE `UTexture2D` objects via the
  object model (no extraction/redistribution needed; load from the live game).

## RESULT of the instrumentation run (decisive)

Live probe on 1.3528 (read-only reflection; `src/ue/UeObject.*`, `RosterProbe.*`):

- UE reflection confirmed working: `GObjects.Num` ~92-93k (grows in-game),
  `FName::AppendString` resolves names correctly.
- The real game-instance class is **`MCCGameInstance` / `SDGameInstance`**, not
  `BP_MCCGameInstance_C` (that was dev-branch lore).
- **Zero** live `MCCGameSessionViewModel` / `MCCRosterViewModel` instances exist
  on the menu or in offline play — they're only built when the online roster UI
  opens.
- The dump confirms `MCCGameSessionViewModel` is the **online party/social
  roster**: `HandleGameInvite`/`GameInvites`, `HandlePrivacySettingsChange`,
  `Friends`, `InviteFriendsButton`, `PartyLeaderLabelText`; owned by
  `MCCGameSessionDetailsWidget`. **No** `LocalPlayer/SplitScreen/Couch/Guest`
  roster viewmodel exists in the MCC namespace.

### Conclusion

There is **no native offline couch-join roster** in MCC PC to inject into.
`MCCGameSessionViewModel.Players` is the wrong (online party) system and isn't
instantiated offline. This confirms the earlier roster spike (Outcome C).

What is already native once players join: the real **splitscreen viewports** and
in-game **`WBP_Nameplate`** player nameplates (game assets). Only the *join
moment* is ours.

### Therefore: "native using game assets" = asset-reuse skin

Skin the working join overlay (`src/mcc/splitscreen/LocalCoop.*`, which drives
the proven backend) with MCC's **real** assets:
- Controller glyph textures (`Data/UI/.../ControllerTexturePack`).
- Shell fonts + colour palette.
- Nameplate panel styling to match `WBP_Nameplate`.
Cost driver: binding MCC `UTexture2D`/packed UI assets into ImGui (D3D11 SRV).

## Status

- Track A (ImGui native-looking screen): built + validated on 1.3528
  (`src/mcc/splitscreen/LocalCoop.*`).
- Track B (this doc): **resolved** — native offline roster does not exist;
  pivot to asset-reuse skinning of the join overlay.
