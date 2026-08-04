# Pirate Signals

Pirate Signals is an independent, server-authoritative public chat mod for
Windrose dedicated servers. Messages use the game's existing client-to-server
connection. The mod does not use an external relay, domain, account system,
extra listening port, or third-party chat service.

This project was developed independently. It does not contain or use the
third-party WindroseChat mod's code, binary, protocol, relay, or identity
logic.

## Current behavior

- Public server-wide chat only; no private messages or rooms.
- Dedicated server supplies the visible player name from `PlayerState`.
- The chat protocol does not submit Steam IDs, account IDs, IP addresses, or
  client-selected names.
- History is limited to 100 messages in server memory and clears when the
  dedicated-server process restarts.
- Joining players receive the current bounded history.
- Messages are limited to 300 Unicode code points and 1024 UTF-8 bytes.
- Per-player limit of five accepted messages per ten seconds.
- Exact protocol version matching is required.
- `F8` cycles hidden, latest line, expanded chat, then hidden again.
- `Enter` sends while expanded.

See [Architecture](docs/ARCHITECTURE.md), [Privacy](PRIVACY.md), and
[Security](SECURITY.md) for details.

## Repository layout

```text
src/client/       Client UE4SS Lua controller
src/server/       Dedicated-server UE4SS Lua controller
src/native/       Native DirectX 12 / Dear ImGui client overlay
transport/        LogicMods loader configuration
docs/             Architecture and build notes
licenses/         Third-party license texts
```

The cooked Unreal transport containers and compiled DLL are release artifacts,
not source files, and are intentionally excluded from the source tree. Public
release archives should include SHA-256 checksums.

## Installation

The release archive contains the client folders at its root and the dedicated-
server folders under `Server`:

```text
Pirate Signals/
|-- PirateSignals/                 client UE4SS mod
|-- PirateSignalsTransport/        client transport
`-- Server/
    |-- PirateSignals/             dedicated-server UE4SS mod
    `-- PirateSignalsTransport/    dedicated-server transport
```

For the client, copy the root `PirateSignals` folder to `ue4ss/Mods` and the
root `PirateSignalsTransport` folder to `R5/Content/Paks/LogicMods`.

For the dedicated server, copy `Server/PirateSignals` to the server's
`ue4ss/Mods` folder and `Server/PirateSignalsTransport` to the server's
`R5/Content/Paks/LogicMods` folder.

## License

Pirate Signals is licensed under the [MIT License](LICENSE). Independent
third-party components retain their original licenses; see
[Third-party notices](THIRD_PARTY_NOTICES.md).
