#pragma once

#include "Network/BlockDelta.h"
#include "Network/NetTypes.h"
#include "Network/NetworkSnapshot.h"
#include "Network/PlayerCommand.h"
#include "Network/SnapshotDelta.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Binary wire protocol for DaiBed packets (no transport yet — see
// docs/NETWORK_PREP_PLAN.md). This is the serialization layer a future UDP/ENet
// transport will hand bytes to/from; here it only encodes/decodes in memory.
//
// Encoding choice — SIMPLE explicit-size little-endian BINARY:
//   * every integer has a fixed width (u8/u16/u32/i32), little-endian;
//   * every float is its IEEE-754 bit pattern written as u32 (bit-exact, no
//     locale/precision loss like a text format would have);
//   * every bool is one byte (0/1);
//   * variable arrays are length-prefixed with a u32 count.
// No struct memcpy / no compiler padding is relied on, so the format is stable
// and portable across compilers/architectures. Rationale (task step 3): a first
// pass wants a simple binary with explicit sizes; compact text (JSON) would be
// larger, slower, and we already have a JSON path for stats. varint/delta
// compression can come later without changing the call sites.

enum class MessageType : std::uint8_t
{
    Invalid = 0,
    PlayerCommand = 1,
    MatchSnapshot = 2,
    // Transport control messages (Phase 0.1R). Header-only except ConnectAck,
    // which carries the assigned playerId as an i32 payload.
    Connect = 3,     // client -> server: hello / join request (payload: token string)
    ConnectAck = 4,  // server -> client: accepted (payload: i32 assigned playerId)
    Disconnect = 5,  // either direction: graceful leave
    Heartbeat = 6,   // keepalive so the peer's timeout does not fire
    ConnectDenied = 7, // server -> client: rejected (payload: reason string)
    LobbyUpdate = 8,    // client -> server: name/team/hero/ready/start intent
    LobbySnapshot = 9,  // server -> client: authoritative pre-match state
    SnapshotDelta = 10, // server -> client: delta from a compatible baseline
    FullResyncRequest = 11, // client -> server: ask for a fresh full baseline
    ReliableAck = 12, // either direction: ack a reliable control/baseline packet
    PlayerCommandBatch = 13, // client -> server: recent commands, ordered oldest -> newest
    PacketFragment = 14, // either direction: one chunk of an oversized packet (see Encode below)
};

const char* ToString(MessageType type);

// Protocol versioning. Bump on ANY wire-format change; decoders reject a
// mismatch (VersionMismatch) instead of silently misparsing.
constexpr std::uint16_t kProtocolVersion = 23;
// Leading magic so random/foreign bytes are rejected cleanly as BadMagic.
constexpr std::uint32_t kProtocolMagic = 0x3142'4400u; // "DB1\0"
// Header size on the wire: magic(4) + version(2) + type(1) + sequence(4) +
// tick(4) + payloadSize(4).
constexpr std::size_t kPacketHeaderSize = 19;

// Fixed prefix on every packet.
struct PacketHeader
{
    std::uint32_t magic = kProtocolMagic;
    std::uint16_t protocolVersion = kProtocolVersion;
    MessageType type = MessageType::Invalid;
    std::uint32_t sequence = 0;
    std::uint32_t tick = 0;
    std::uint32_t payloadSize = 0;
};

// Outcome of decoding. Anything other than Ok means "do not trust the payload";
// none of these ever throw or read out of bounds.
enum class DecodeStatus
{
    Ok,
    TooShort,        // fewer bytes than the header/payload needs (truncated)
    BadMagic,        // not our protocol at all (random/foreign bytes)
    VersionMismatch, // our magic, different protocol version
    WrongType,       // header is fine but not the message the caller asked for
    BadPayload,      // payload ran out / failed to parse
};

const char* ToString(DecodeStatus status);

// --- Encoders: build a complete packet (header + payload) ---------------------
std::vector<std::uint8_t> EncodePlayerCommand(std::uint32_t sequence, const PlayerCommand& command);
std::vector<std::uint8_t> EncodePlayerCommandBatch(
    std::uint32_t sequence, const std::vector<PlayerCommand>& commands);
std::vector<std::uint8_t> EncodeMatchSnapshot(std::uint32_t sequence, const MatchSnapshot& snapshot);
std::vector<std::uint8_t> EncodeSnapshotDelta(std::uint32_t sequence, const MatchSnapshotDelta& delta);
// Header-only control packets (Connect / Disconnect / Heartbeat). `type` must be
// one of those; tick is informational.
std::vector<std::uint8_t> EncodeControl(MessageType type, std::uint32_t sequence, std::uint32_t tick);
// Connect carries the join token (server password); empty token == no password.
std::vector<std::uint8_t> EncodeConnect(std::uint32_t sequence, const std::string& token);
DecodeStatus DecodeConnect(const std::uint8_t* data, std::size_t size, PacketHeader& header, std::string& token);
// ConnectAck carries the playerId the server assigned to the client.
std::vector<std::uint8_t> EncodeConnectAck(std::uint32_t sequence, int assignedPlayerId);
DecodeStatus DecodeConnectAck(const std::uint8_t* data, std::size_t size, PacketHeader& header, int& assignedPlayerId);
// ConnectDenied carries a human-readable reason (e.g. "bad password").
std::vector<std::uint8_t> EncodeConnectDenied(std::uint32_t sequence, const std::string& reason);
DecodeStatus DecodeConnectDenied(const std::uint8_t* data, std::size_t size, PacketHeader& header, std::string& reason);
std::vector<std::uint8_t> EncodeLobbyUpdate(std::uint32_t sequence, const LobbyUpdate& update);
DecodeStatus DecodeLobbyUpdate(const std::uint8_t* data, std::size_t size, PacketHeader& header, LobbyUpdate& update);
std::vector<std::uint8_t> EncodeLobbySnapshot(std::uint32_t sequence, const LobbySnapshot& snapshot);
DecodeStatus DecodeLobbySnapshot(const std::uint8_t* data, std::size_t size, PacketHeader& header, LobbySnapshot& snapshot);
std::vector<std::uint8_t> EncodeFullResyncRequest(
    std::uint32_t sequence, std::uint32_t lastKnownSequence, std::uint32_t lastKnownTick);
DecodeStatus DecodeFullResyncRequest(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::uint32_t& lastKnownSequence, std::uint32_t& lastKnownTick);
std::vector<std::uint8_t> EncodeReliableAck(
    std::uint32_t sequence, std::uint32_t ackedSequence, MessageType ackedType);
DecodeStatus DecodeReliableAck(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::uint32_t& ackedSequence, MessageType& ackedType);
// PacketFragment: transport-level split of an oversized datagram. Any encoded
// packet larger than the transport's safe-MTU threshold is sent as N fragment
// datagrams instead of one huge UDP datagram (which would be IP-fragmented or
// silently rejected past 64 KB). fragmentId is the ORIGINAL packet's header
// sequence — stable across reliable retries, so reassembly is idempotent.
// Chunks use a FIXED layout (chunk i covers bytes [i*chunkBytes, ...) of the
// original packet), carried as a length-prefixed byte array. The reassembled
// bytes are a complete ordinary packet and re-enter the normal dispatch.
std::vector<std::uint8_t> EncodePacketFragment(
    std::uint32_t sequence, std::uint32_t fragmentId, std::uint16_t fragmentIndex,
    std::uint16_t fragmentCount, std::uint32_t totalSize,
    const std::uint8_t* chunk, std::size_t chunkSize);
DecodeStatus DecodePacketFragment(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::uint32_t& fragmentId, std::uint16_t& fragmentIndex, std::uint16_t& fragmentCount,
    std::uint32_t& totalSize, std::vector<std::uint8_t>& chunk);

// --- Decoders: validate the header, then parse the payload --------------------
// On a non-Ok status `out` is left untouched/partial; `header` is filled as far
// as it was validly read (so VersionMismatch still reports the wire version).
DecodeStatus DecodeHeader(const std::uint8_t* data, std::size_t size, PacketHeader& header);
DecodeStatus DecodePlayerCommand(const std::uint8_t* data, std::size_t size, PacketHeader& header, PlayerCommand& out);
DecodeStatus DecodePlayerCommandBatch(
    const std::uint8_t* data, std::size_t size, PacketHeader& header,
    std::vector<PlayerCommand>& out);
DecodeStatus DecodeMatchSnapshot(const std::uint8_t* data, std::size_t size, PacketHeader& header, MatchSnapshot& out);
DecodeStatus DecodeSnapshotDelta(const std::uint8_t* data, std::size_t size, PacketHeader& header, MatchSnapshotDelta& out);

// Headless self-test (CLI: --protocol-smoke). Roundtrips a PlayerCommand and a
// MatchSnapshot (header + PlayerSnapshots + CoreSnapshots + BlockDeltas), and
// checks that truncated / bad-magic / version-mismatch packets are rejected
// without crashing. Returns a process exit code (0 == success).
int RunProtocolSmoke();
