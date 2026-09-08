# P2P implementation status

The multiplayer protocol is host-authoritative: clients submit input commands,
while the host owns simulation and sends snapshots. P2P here means replacing
the datagram delivery mechanism, not moving authority to every peer.

## Completed foundation

- `IDatagramBackend` isolates peer discovery/delivery from the wire protocol.
- `CreateDatagramBackend(NetworkBackend)` is the only backend construction
  point. `ServerConfig` selects the host backend, while clients select one in
  the `ClientTransport` constructor.
- `UdpDatagramBackend` contains all Winsock/BSD socket code. The session,
  reliability, fragmentation, lobby, commands, and snapshots use only opaque
  `DatagramPeer` handles.
- `DAIBED_ENABLE_NETWORK=OFF` builds the same interface with a safe disabled
  backend.
- Protocol v26 issues a 128-bit `SessionId`, `PlayerSessionId`, and bearer
  `ReconnectToken` during `ConnectAck`.
- A locked match accepts reconnect only when all three credentials match a
  live reservation. Display names are never used as identity.
- Reconnect tokens rotate after every successful reconnect and reservations
  expire after 120 seconds.
- A valid credential set can rebind a live channel to a changed opaque endpoint
  (for example after NAT rebinding) without waiting for the old endpoint timeout;
  this path rotates the bearer token as well.
- `InProcessP2P` is a dependency-free provider-contract backend. It maintains
  opaque peer handles and message queues in a process-wide hub, deliberately
  exercising the full session protocol without IPv4 addresses or OS sockets.
- Provider-authenticated identity is bound to every P2P session reservation.
  A stolen reconnect token cannot reclaim a slot from a different Steam user;
  UDP remains compatible and continues to use the session credentials alone.

`--mp-loopback-smoke` verifies a legitimate reconnect, denial of a new client
trying the old name-based path, denial of a modified token, token rotation, and
a full snapshot baseline followed by deltas.

`--datagram-backend-smoke` checks listener collisions, missing sessions, closed
peers, bidirectional datagram delivery, then runs Connect/ConnectAck, lobby,
commands, and snapshots through `InProcessP2P`. It is a contract test, not an
internet transport and not a NAT/relay emulator.

## Steam Networking Sockets backend

`SteamP2P` is now a provider-specific `IDatagramBackend`. It uses
`CreateListenSocketP2P` / `ConnectP2P`, accepts incoming connections from Steam
callbacks, assigns them to a poll group, and sends the existing 1200-byte
transport datagrams with `k_nSteamNetworkingSend_UnreliableNoNagle`. Reliability,
fragmentation, sessions, reconnect tokens, lobby state, commands, and snapshots
remain in the existing DaiBed protocol.

The player release is universal: the same executable contains native UDP and
Steam P2P. At startup it prefers Steam when the Steam client and authenticated
user are available, and otherwise automatically uses UDP. Explicit
`--network-backend steam` and `--network-backend udp` flags still override the
automatic choice.

The Steamworks SDK is not publicly downloadable, so place it in the ignored
`sdk/` directory. CMake detects that local SDK and enables Steam support in the
normal release build automatically:

```powershell
cmake -S . -B build-release -G "Visual Studio 17 2022" -A x64
cmake --build build-release --config Release
cmake --install build-release --config Release --prefix build-release/package
```

For an SDK stored elsewhere, set `DAIBED_STEAMWORKS_SDK_ROOT` explicitly. A
dependency-free tooling/CI build can opt out with
`-DDAIBED_ENABLE_STEAMWORKS=OFF`; it is not a separate player release.

The universal package includes the matching redistributable Steam API runtime
next to `DaiBed.exe`. A development-only `steam_appid.txt` is generated in the
build output. Its cache setting is `DAIBED_STEAM_APP_ID` and currently defaults
to Valve Spacewar (`480`). The file is not installed or packaged and must not
be uploaded to a Steam depot.

With Steam running, the universal interactive build selects Steam P2P and
virtual port 777 by default. Without Steam it selects UDP and port 7777. The
normal player flow requires no command line:

1. Open **Multiplayer -> Host**, configure the match, and choose **Create and join**.
2. DaiBed starts the authoritative listen-server and creates a Steam Lobby.
3. In the in-game lobby, the host chooses **Invite Steam friends** (or presses F2).
4. DaiBed opens its own scrollable Steam friend picker; the host selects a friend.
5. Accepting the invite joins the Steam Lobby, reads its authenticated host ID
   and virtual port metadata, and connects the normal DaiBed snapshot client.

The friend picker sends the invitation with `InviteUserToLobby`, so the host
does not need Steam Overlay enabled. Overlay remains optional. The recipient
still needs the Steam client running so the lobby invitation and callback can
be delivered.

If the game was closed when the invitation was accepted, Steam launches it with
`+connect_lobby <lobby-id>`; DaiBed reads that through
`ISteamApps::GetLaunchCommandLine` and follows the same in-game join path. A
password-protected lobby opens the Join screen and asks only for the password.
Cold-start invitation launching requires DaiBed's own App ID and launch options
to be configured in Steamworks. With shared development App ID 480, both testers
should start DaiBed first; accepting an invite then arrives through the live
`GameLobbyJoinRequested_t` callback.

The following commands remain developer diagnostics, not the player UX. Steam
uses a small virtual P2P port in the documented range 0..999 (777 below):

```powershell
# Verify Steam initialization and lobby creation with one account
DaiBed.exe --steam-lobby-smoke

# Low-level direct connection fallback
DaiBed.exe --connect 7656119XXXXXXXXXX:777 --network-backend steam
```

`steam:7656119XXXXXXXXXX:777` is also accepted. The listen address is ignored
by Steam; `port` is a virtual port rather than a UDP port.

The current GUI Host flow launches the authoritative server in a second process
for UDP. With `SteamP2P`, it instead creates a second, headless `Game` object in
the host game's process. That object owns the authoritative world and
`ServerTransport`; the visible `Game` remains an ordinary snapshot client. Both
ends and all Steam callbacks are pumped serially from the main thread. The join
host's local client/server path uses `CreateSocketPair` instead of trying to
establish an SDR route back to the same Steam identity. The join address is
filled with the authenticated local Steam ID and the copied join command
includes `--network-backend steam`.

Client connection setup is frame-driven: opening a host or accepting an invite
no longer runs a synchronous polling loop on the rendering thread. Steam gets a
20-second initial connection window (UDP keeps 5 seconds), while the UI remains
responsive and allows cancellation with Escape. `InitRelayNetworkAccess` is
called when the Steam runtime starts to warm relay routing and certificates
before the first remote connection. Provider failures now include Steam's
detailed connection status in the in-game error instead of collapsing every
failure into the same generic timeout.

The runtime uses a process-wide reference-counted Steam API lease shared by the
listen socket and local client. This follows the useful lifecycle and identity
lessons from [e4steam](https://github.com/Kamilhik/e4steam) without copying its
application-specific protocol: DaiBed
keeps its existing host-authoritative packets, reconnect tokens, lobby state,
fragmentation, and snapshot logic. A true headless server still needs Steam
Game Server authentication and its dedicated-server/SDR ticket flow.

`--integrated-server-smoke` now also starts this production listen-server shape
through `InProcessP2P`, joins a client, receives a lobby snapshot, and verifies
clean teardown. An SDK-enabled build and two real Steam accounts are still
required for the final relay/NAT runtime test.

## Remaining provider work

The transport backend, GUI listen-server, Steam Lobby creation, in-game friend
invitation, invite callback, cold-start `+connect_lobby` handling, metadata-based
endpoint discovery, authenticated reconnect binding, connection errors, and
teardown are implemented. A browsable public lobby list and richer friend
presence remain future UX work. No gameplay packet codec changes are required.
Native UDP remains the LAN/localhost backend and test oracle.
