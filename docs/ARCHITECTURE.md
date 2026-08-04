# Architecture

Pirate Signals uses one server-created, player-owned replicated Unreal actor
per connection. Reliable Unreal RPCs carry small versioned frames over the
game's existing network connection.

```text
Client Lua/UI -> owned transport actor -> dedicated-server Lua
Dedicated-server validation/history -> owned transport actors -> all clients
```

The server verifies that a submission arrived through a transport actor owned
by a connected player. It derives the visible identity from that player's
server-side `PlayerState`, validates the text and rate limit, adds the message
to bounded memory history, and broadcasts it.

Protocol version 1 uses the `WRCHAT` internal namespace. The cooked asset path
still contains `WindroseChatTransport` as a compatibility identifier from the
private feasibility phase; it is not a connection to, dependency on, or reuse
of the unrelated third-party WindroseChat mod.

## Frame types

Client to server:

```text
WRCHAT|1|HELLO|<client-session>
WRCHAT|1|SUBMIT|<increasing-sequence>|<percent-encoded-message>
```

Server to client:

```text
WRCHAT|1|HISTORY_BEGIN|<count>|<last-id>
WRCHAT|1|HISTORY_ITEM|<id>|<epoch>|<encoded-name>|<encoded-message>
WRCHAT|1|HISTORY_END|<last-id>
WRCHAT|1|MESSAGE|<id>|<epoch>|<encoded-name>|<encoded-message>
WRCHAT|1|ERROR|<code>|<encoded-detail>
```

The native DLL renders the overlay with Dear ImGui through the DirectX 12
presentation path. Lua owns network discovery, RPC framing, validation state,
and the bridge to the native UI.
