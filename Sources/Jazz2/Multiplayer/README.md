# Multiplayer — PSP ad-hoc build

Multiplayer **is** built into the PSP EBOOT: `WITH_MULTIPLAYER` is defined and
the game-logic translation units in this directory are listed in
`../../../cmake/psp_sources.cmake` (`NetworkManager.cpp`, `NetworkManagerBase.cpp`,
`Peer.cpp`, `MpLevelHandler.cpp`, `GameModes/*.cpp`, `RaceRouteGenerator.cpp`,
`ConnectionResult.cpp`). The transport is PSP ad-hoc WLAN.

## Transport

The concrete transport is `nCine::PspAdhoc`
(`../../nCine/Backends/Psp/PspAdhoc.cpp`), built on the PSP `pspnet_adhoc*` /
`pspwlan` libraries. `NetworkManagerBase` attaches to a `PspAdhoc` instance
(`AttachAdhoc`) and drives connect/host/send/poll through it. There is no ENet,
ixwebsocket or BSD-socket transport in this tree — those desktop backends
(and the ENet LAN-broadcast `ServerDiscovery`) were not migrated to PSP and
have been removed. Peer discovery on PSP happens through the ad-hoc/WLAN
scan in `PspAdhoc` plus the discovery packets in `PacketTypes.h`.

## Layers (platform-independent)

* **Interfaces / boundaries** — `INetworkHandler.h`, `NetworkManagerBase.h`,
  `MpGameMode.h`, `GameModes/IGameMode.h`, `GameModes/MpPlayerState.h`.
* **Wire format & protocol** — `PacketTypes.h` (packet/message enums),
  `Reason.h`, `ConnectionResult.*`, `PeerDescriptor.h`, `ServerInitialization.h`,
  `Teams.h`.
* **Session / player / peer state** — `Peer.*`, `MpLevelHandler.*`.
* **Game-mode logic** (rules, scoring, team assignment) —
  `GameModes/BattleMode`, `CaptureTheFlagMode`, `CooperationMode`, `RaceMode`,
  `TeamBattleMode`, `TreasureHuntMode`, `GameModeFactory`, plus the header-only
  `TeamRaceMode.h` / `TeamTreasureHuntMode.h`, and `RaceRouteGenerator.*`.
