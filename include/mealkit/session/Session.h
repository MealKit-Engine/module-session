#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

// The match session as the game sees it (docs/ENGINE_MODULES.md "Multiplayer as a module").
// Game code (GameContext::session, GameInstance::lobby, headless bot hooks, game::Simulation) uses only session::Session and the
// types here, so it doesn't depend on the net module. net::LockstepSession implements it (lockstep over UDP or an online transport,
// and the offline/replay path). Setting up a session (host, join, transports, network conditions) stays on the implementation,
// which game::MatchService owns.
//
// Per tick: canExecute(t) → inputs(t) → run the tick → tickExecuted(t, hash) → submitLocalInput(t + inputDelay, input).
// Confirmed commands (submitCommand) ride with a confirmed tick's inputs; read them with forEachCommand (games use sim/Commands.h).
namespace mealkit::session {

inline constexpr u32 kMaxPlayers = 8;
inline constexpr u32 kInputBytes = 8;   // player input size defined by the game (per tick)
inline constexpr u32 kTickWindow = 256; // range of ticks that can be handled at once (about 8.5 seconds at 30Hz)
inline constexpr u32 kMaxCommandBytes = 200;     // max size of one command
inline constexpr u32 kCommandBytesPerTick = 256; // commands carried per tick (including a 2-byte header per command)
inline constexpr u32 kMaxLoadoutBytes = 128;     // match start data per player (so 8 players fit in one start packet)
inline constexpr u32 kMaxNameBytes = 24;         // player name shown in the lobby (UTF-8, cut by character if longer)
inline constexpr u32 kMaxChatBytes = 120;        // one chat line (UTF-8)
inline constexpr u32 kChatLogSize = 50;          // number of lines chatLog() keeps
inline constexpr f64 kChatInterval = 0.5;        // minimum interval between my chat lines (seconds)
// Engine confirmed command name (command type = fnv1a of the name): player connect/disconnect. Contents [connected u8][loadout bytes], sending player = that slot
inline constexpr const char* kPresenceCommand = "mealkit.net.Presence";

struct PlayerInput {
    u8 bytes[kInputBytes] = {};
};

struct TickInputs {
    PlayerInput players[kMaxPlayers];
    u16 commandBytes = 0;                   // bytes used in commands
    u8 commands[kCommandBytesPerTick] = {}; // repeated [player u8][size u8][contents] (read with forEachCommand)
};

// This tick's confirmed commands: f(player, contents, size)
template <class F>
void forEachCommand(const TickInputs& in, F&& f) {
    u32 i = 0;
    while (i + 2 <= in.commandBytes) {
        const u8 player = in.commands[i];
        const u32 size = in.commands[i + 1];
        if (i + 2 + size > in.commandBytes) break;
        f(player, in.commands + i + 2, size);
        i += 2 + size;
    }
}

enum class SessionState : u8 {
    Idle,       // before start
    Connecting, // client: connecting to the host
    Lobby,      // waiting for match start
    Running,    // simulation in progress
    Ended,      // ended (see endReason)
};

struct SessionStats {
    f32 rttMs = 0.0f; // client: round-trip time to the host
    u32 sentBytesPerSec = 0;
    u32 recvBytesPerSec = 0;
};

// Device a player plays with (shown in the lobby; crossplay matches mix platforms). Information only: the simulation sees quantized input
enum class InputKind : u8 { KeyboardMouse, Gamepad, Touch };
const char* inputKindName(InputKind kind); // "keyboard/mouse", "gamepad", "touch" (English, logs)

// One person in the lobby (collected by the host and distributed via lobby signals)
struct LobbyMember {
    u8 player = 0; // player index (host 0). Renumbered without gaps when the match starts
    std::string name;
    bool ready = false;
    bool host = false;
    InputKind input = InputKind::KeyboardMouse;
    u32 pingMs = 0;          // round-trip time to the host (measured by the host, 0 for the host itself)
    std::vector<u8> loadout; // match start data that person picked (read with game::readLobbyLoadout)
};

struct ChatMessage {
    u32 serial = 0; // order received in this session (from 1): lets the screen recognize new lines
    u8 player = 0;
    std::string name; // name at send time
    std::string text;
};

// Content hash components (compared separately to report rejection reasons): order of game::MatchData::contentParts
enum class ContentPart : u8 { Scene, Models, Tuning, Project, Tables, Count };

// Connection rejection reason (client, rejectInfo())
// Input: the host only takes players with its own input kind. Crossplay: another OS and one side turned crossplay off
enum class RejectReason : u8 { None, Version, Platform, Content, Full, InProgress, Input, Crossplay };
struct RejectInfo {
    RejectReason reason = RejectReason::None;
    u32 contentMask = 0;      // Content: differing components (bit = ContentPart)
    std::string hostVersion;  // Version: the host's game version
    std::string hostGroup;    // Platform: the host's determinism group
    std::string hostPlatform; // Crossplay: the host's OS
    InputKind hostInput = InputKind::KeyboardMouse; // Input: the host's input kind
};

// Cuts a UTF-8 string to at most maxBytes on character boundaries (names, chat)
std::string clampUtf8(const std::string& text, u32 maxBytes);

// Automatic input delay adjustment (host): set at match start from round-trip times measured in the lobby, and changed 1 tick at a time mid-match from the same tick for everyone via an engine confirmed command
struct AutoDelay {
    bool enabled = false;
    f32 tickMs = 1000.0f / 30.0f; // length of one tick (project tick rate)
    u32 minTicks = 2;             // never shorter than this, even on the same router
    u32 maxTicks = 8;             // peers farther than this stutter (the lobby warns)
};

// Input delay (ticks) for this round-trip time (ms): time for the farthest peer's input to reach everyone via the host + jitter margin + one tick
u32 delayForRtt(f32 rttMs, const AutoDelay& autoDelay);

// Value that differs per executable (used instead when the simulation version is unknown)
u32 buildId();
// Determinism group: compiler and CPU ("msvc-x64", "clang-x64", "clang-arm64" ...). Until cross-platform determinism is verified (stage 5)
// peers in a different group are rejected
const char* determinismGroup();
// Operating system ("windows", "linux", "macos", ...): players who turn crossplay off only meet peers of the same one
const char* platformName();

class Session {
public:
    virtual ~Session() = default;

    // ---- State
    virtual SessionState state() const = 0;
    virtual const char* endReason() const = 0;
    virtual const RejectInfo& rejectInfo() const = 0; // client: why the host rejected the connection (after Ended)
    virtual bool isHost() const = 0;
    virtual bool isOffline() const = 0;
    virtual bool isReplay() const = 0;
    virtual u32 replayTicks() const = 0; // number of recorded ticks (0 if not a replay)
    virtual u8 localPlayer() const = 0;
    virtual u32 playerCount() const = 0;      // match player count (confirmed after Running)
    virtual u32 lobbyPlayerCount() const = 0; // lobby player count
    virtual u32 maxPlayers() const = 0;
    virtual u64 seed() const = 0;
    virtual u32 matchData() const = 0;
    virtual u32 matchIndex() const = 0; // number of matches started (the first match is 1)
    virtual u32 inputDelay() const = 0;
    virtual u64 contentHash() const = 0;
    virtual bool crossplay() const = 0;
    virtual u32 hostMigrations() const = 0;
    virtual const SessionStats& stats() const = 0;
    virtual u32 pingMs(u32 player) const = 0; // round-trip time to the host (0 for the host)
    // The match is stalled waiting for these players' inputs (bit = player index, only when stalled over 0.15 s)
    virtual u32 waitingFor() const = 0;
    virtual bool desynced() const = 0;
    virtual u32 desyncTick() const = 0;

    // ---- Lobby (names, ready, input kind, loadouts, chat)
    virtual void setLocalName(const std::string& name) = 0; // up to kMaxNameBytes
    virtual const std::string& localName() const = 0;
    virtual void setLocalReady(bool ready) = 0;
    virtual bool localReady() const = 0;
    virtual void setLocalInputKind(InputKind kind) = 0;
    virtual InputKind localInputKind() const = 0;
    virtual const std::vector<LobbyMember>& lobbyMembers() const = 0; // player index order, including the host
    // Match start data (game/Loadout.h): up to kMaxLoadoutBytes, false if over. Confirmed values per player after Running
    virtual bool setLocalLoadout(const void* data, u32 size) = 0;
    virtual const std::vector<u8>& loadout(u32 player) const = 0;
    // One line, cut to kMaxChatBytes; false for empty lines or faster than kChatInterval. Not in input recordings
    virtual bool sendChat(const std::string& text) = 0;
    virtual const std::vector<ChatMessage>& chatLog() const = 0; // received order, the last kChatLogSize lines
    virtual void startMatch(u64 seed, u32 matchData) = 0; // host only: lobby → running
    virtual void returnToLobby() = 0;                     // host only: running → lobby

    // ---- Ticks
    virtual bool canExecute(u32 tick) const = 0;
    virtual const TickInputs& inputs(u32 tick) const = 0;
    virtual void tickExecuted(u32 tick, u64 stateHash) = 0; // right after running the tick
    virtual void submitLocalInput(u32 tick, const PlayerInput& input) = 0;
    // Confirmed command (only Running, size 1..kMaxCommandBytes): goes into the next confirmed tick. false if the queue is full
    virtual bool submitCommand(const void* data, u32 size) = 0;
    virtual u32 connectedMask() const = 0; // players connected during the match (bit = player index, as of executed ticks)

    // ---- Late join (host provides a snapshot, the joiner fast-forwards)
    virtual bool snapshotWanted() const = 0;
    virtual void provideSnapshot(u32 tick, const std::vector<u8>& bytes) = 0;
    virtual u32 joinTick() const = 0;
    virtual bool takeJoinSnapshot(std::vector<u8>& out) = 0;
    virtual bool catchingUp() const = 0;
};

} // namespace mealkit::session
