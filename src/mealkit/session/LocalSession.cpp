#include "mealkit/session/LocalSession.h"

#include "mealkit/core/Hash.h"
#include "mealkit/core/Log.h"

#include <algorithm>
#include <cstring>

// Mirrors net::LockstepSession's offline host path (Lockstep.cpp): keep the two in step, replays and saved matches depend on it.
namespace mealkit::session {
namespace {

const u32 kDelayCommandType = static_cast<u32>(fnv1a("mealkit.net.InputDelay")); // same engine command as net
constexpr u32 kMaxPendingCommands = 256;
constexpr u32 kNoTick = 0xFFFFFFFFu;

struct Slot {
    u32 tick = kNoTick;
    bool complete = false;
    TickInputs inputs{};
};

struct Command {
    u8 size = 0;
    u8 data[kMaxCommandBytes];
};

} // namespace

struct LocalSession::Impl {
    SessionState state = SessionState::Idle;
    bool replay = false;
    u32 playerCount = 1;
    u64 seed = 0;
    u32 matchData = 0;
    u32 matchIndex = 0;
    u32 inputDelay = 1;
    u64 contentHash = 0;
    bool crossplay = true;
    std::string localName;
    bool localReady = false;
    InputKind localInput = InputKind::KeyboardMouse;
    std::vector<u8> localLoadout;
    std::vector<u8> loadouts[kMaxPlayers];
    std::vector<LobbyMember> members;
    std::vector<ChatMessage> chatLog;
    u32 chatSerial = 0;
    f64 now = 0.0;
    f64 lastLocalChat = -1e9;
    Slot slots[kTickWindow];
    u32 finalNext = 0;
    std::vector<Command> pendingCommands;
    bool delayChangePending = false;
    PlayerInput lastLocalInput{};
    std::vector<TickInputs> replayInputs;
    std::vector<u64> replayHashes;
    bool desynced = false;
    u32 desyncTick = 0;
    RejectInfo rejectInfo;
    SessionStats stats;

    Slot& slot(u32 tick) { return slots[tick % kTickWindow]; }
    const Slot& slot(u32 tick) const { return slots[tick % kTickWindow]; }
    Slot& claim(u32 tick) {
        Slot& s = slot(tick);
        if (s.tick != tick) {
            s = Slot{};
            s.tick = tick;
        }
        return s;
    }

    std::string displayName() const { return localName.empty() ? std::string("P1") : localName; }

    void rebuildMembers() {
        members.clear();
        LobbyMember self;
        self.name = displayName();
        self.ready = true;
        self.host = true;
        self.input = localInput;
        self.loadout = localLoadout;
        members.push_back(std::move(self));
    }

    void beginRunning() {
        for (Slot& s : slots) s = Slot{};
        std::fill(std::begin(filledTick), std::end(filledTick), kNoTick);
        for (u32 t = 0; t < inputDelay; ++t) { // the first inputDelay ticks are confirmed with empty input
            Slot& s = claim(t);
            s.complete = true;
        }
        finalNext = inputDelay;
        desynced = false;
        desyncTick = 0;
        pendingCommands.clear();
        delayChangePending = false;
        lastLocalInput = {};
        state = SessionState::Running;
    }

    // Commands go into the tick being confirmed, in send order (net: player index first, and there is only player 0)
    void packCommands(TickInputs& in) {
        in.commandBytes = 0;
        size_t taken = 0;
        for (; taken < pendingCommands.size(); ++taken) {
            const Command& c = pendingCommands[taken];
            if (in.commandBytes + 2u + c.size > kCommandBytesPerTick) break;
            in.commands[in.commandBytes] = 0;
            in.commands[in.commandBytes + 1] = c.size;
            std::memcpy(in.commands + in.commandBytes + 2, c.data, c.size);
            in.commandBytes = static_cast<u16>(in.commandBytes + 2u + c.size);
        }
        pendingCommands.erase(pendingCommands.begin(), pendingCommands.begin() + static_cast<std::ptrdiff_t>(taken));
    }

    // A slot with my input is complete: confirm it and the ones after it
    void finalize() {
        for (;;) {
            Slot& s = slot(finalNext);
            if (s.tick != finalNext) break;
            if (!s.complete) {
                if (!filled(finalNext)) break; // my input is the only one needed
                packCommands(s.inputs);
                s.complete = true;
            }
            ++finalNext;
        }
    }

    void applyDelay(u32 tick, u32 next) {
        next = std::clamp(next, 1u, 30u);
        if (next == inputDelay) return;
        MEALKIT_INFO("Input delay %u -> %u ticks from tick %u", inputDelay, next, tick);
        for (u32 t = tick + inputDelay; t < tick + next; ++t) { // the gap gets my last input
            Slot& s = claim(t);
            if (!s.complete && !filled(t)) {
                s.inputs.players[0] = lastLocalInput;
                markFilled(t);
            }
        }
        inputDelay = next;
        delayChangePending = false;
        finalize();
    }

    // Whether my input for a not yet confirmed tick arrived (net keeps a mask per slot)
    u32 filledTick[kTickWindow];
    bool filled(u32 tick) const { return filledTick[tick % kTickWindow] == tick; }
    void markFilled(u32 tick) { filledTick[tick % kTickWindow] = tick; }

    Impl() { std::fill(std::begin(filledTick), std::end(filledTick), kNoTick); }
};

LocalSession::LocalSession() : m(std::make_unique<Impl>()) {}
LocalSession::~LocalSession() = default;

bool LocalSession::openOffline() {
    if (m->state != SessionState::Idle) return false;
    m->playerCount = 1;
    m->inputDelay = 1;
    m->state = SessionState::Lobby;
    m->rebuildMembers();
    return true;
}

bool LocalSession::startOffline(u64 seed, u32 matchData) {
    if (!openOffline()) return false;
    startMatch(seed, matchData);
    return true;
}

bool LocalSession::startReplay(u64 seed, u32 matchData, const std::vector<std::vector<u8>>& loadouts,
                               std::vector<TickInputs> inputs, std::vector<u64> hashes) {
    if (m->state != SessionState::Idle) return false;
    const u32 players = std::clamp(static_cast<u32>(loadouts.size()), 1u, kMaxPlayers);
    m->replay = true;
    m->playerCount = players;
    m->inputDelay = 1;
    m->seed = seed;
    m->matchData = matchData;
    for (std::vector<u8>& l : m->loadouts) l.clear();
    for (u32 i = 0; i < players && i < loadouts.size(); ++i)
        m->loadouts[i].assign(loadouts[i].begin(),
                              loadouts[i].begin() + std::min<size_t>(loadouts[i].size(), kMaxLoadoutBytes));
    m->replayInputs = std::move(inputs);
    m->replayHashes = std::move(hashes);
    m->rebuildMembers();
    ++m->matchIndex;
    m->beginRunning();
    MEALKIT_INFO("Replaying a match: %u tick(s), %u player(s), seed %016llx", static_cast<u32>(m->replayInputs.size()),
                 players, static_cast<unsigned long long>(seed));
    return true;
}

bool LocalSession::requestInputDelay(u32 ticks) {
    if (m->state != SessionState::Running || m->replay || m->pendingCommands.size() >= kMaxPendingCommands) return false;
    Command c;
    c.size = 5;
    std::memcpy(c.data, &kDelayCommandType, 4);
    c.data[4] = static_cast<u8>(std::clamp(ticks, 1u, 30u));
    m->pendingCommands.push_back(c);
    m->delayChangePending = true;
    return true;
}

void LocalSession::setContentHash(u64 hash) { m->contentHash = hash; }
void LocalSession::setCrossplay(bool allowed) { m->crossplay = allowed; }

void LocalSession::update(f64 now) {
    m->now = now;
    if (m->state != SessionState::Idle) m->rebuildMembers();
}

void LocalSession::reset() {
    auto fresh = std::make_unique<Impl>();
    fresh->contentHash = m->contentHash;
    fresh->crossplay = m->crossplay;
    fresh->localName = m->localName;
    fresh->localLoadout = m->localLoadout;
    fresh->localInput = m->localInput;
    fresh->now = m->now;
    m = std::move(fresh);
}

SessionState LocalSession::state() const { return m->state; }
const char* LocalSession::endReason() const { return ""; }
const RejectInfo& LocalSession::rejectInfo() const { return m->rejectInfo; }
bool LocalSession::isHost() const { return m->state != SessionState::Idle; }
bool LocalSession::isOffline() const { return m->state != SessionState::Idle; }
bool LocalSession::isReplay() const { return m->replay; }
u32 LocalSession::replayTicks() const { return m->replay ? static_cast<u32>(m->replayInputs.size()) : 0; }
u8 LocalSession::localPlayer() const { return 0; }
u32 LocalSession::playerCount() const { return m->playerCount; }
u32 LocalSession::lobbyPlayerCount() const { return 1; }
u32 LocalSession::maxPlayers() const { return m->playerCount; }
u64 LocalSession::seed() const { return m->seed; }
u32 LocalSession::matchData() const { return m->matchData; }
u32 LocalSession::matchIndex() const { return m->matchIndex; }
u32 LocalSession::inputDelay() const { return m->inputDelay; }
u64 LocalSession::contentHash() const { return m->contentHash; }
bool LocalSession::crossplay() const { return m->crossplay; }
u32 LocalSession::hostMigrations() const { return 0; }
const SessionStats& LocalSession::stats() const { return m->stats; }
u32 LocalSession::pingMs(u32) const { return 0; }
u32 LocalSession::waitingFor() const { return 0; }
bool LocalSession::desynced() const { return m->desynced; }
u32 LocalSession::desyncTick() const { return m->desyncTick; }

void LocalSession::setLocalName(const std::string& name) { m->localName = clampUtf8(name, kMaxNameBytes); }
const std::string& LocalSession::localName() const { return m->localName; }
void LocalSession::setLocalReady(bool ready) { m->localReady = ready; }
bool LocalSession::localReady() const { return m->localReady; }
void LocalSession::setLocalInputKind(InputKind kind) { m->localInput = kind; }
InputKind LocalSession::localInputKind() const { return m->localInput; }
const std::vector<LobbyMember>& LocalSession::lobbyMembers() const { return m->members; }

bool LocalSession::setLocalLoadout(const void* data, u32 size) {
    if (size > kMaxLoadoutBytes || m->state == SessionState::Running || m->state == SessionState::Ended) return false;
    const u8* bytes = static_cast<const u8*>(data);
    m->localLoadout.assign(bytes, bytes + size);
    return true;
}

const std::vector<u8>& LocalSession::loadout(u32 player) const {
    static const std::vector<u8> none;
    return player < kMaxPlayers ? m->loadouts[player] : none;
}

bool LocalSession::sendChat(const std::string& text) {
    if (m->state != SessionState::Lobby && m->state != SessionState::Running) return false;
    const std::string line = clampUtf8(text, kMaxChatBytes);
    if (line.empty() || m->now - m->lastLocalChat < kChatInterval) return false;
    m->lastLocalChat = m->now;
    ChatMessage entry;
    entry.serial = ++m->chatSerial;
    entry.player = 0;
    entry.name = m->displayName();
    entry.text = line;
    m->chatLog.push_back(std::move(entry));
    if (m->chatLog.size() > kChatLogSize) m->chatLog.erase(m->chatLog.begin());
    return true;
}

const std::vector<ChatMessage>& LocalSession::chatLog() const { return m->chatLog; }

void LocalSession::startMatch(u64 seed, u32 matchData) {
    if (m->state != SessionState::Lobby) return;
    m->rebuildMembers();
    m->playerCount = 1;
    for (std::vector<u8>& l : m->loadouts) l.clear();
    m->loadouts[0] = m->localLoadout;
    m->seed = seed;
    m->matchData = matchData;
    ++m->matchIndex;
    m->beginRunning();
    MEALKIT_INFO("Match %u started with 1 player(s), seed %016llx", m->matchIndex, static_cast<unsigned long long>(seed));
}

void LocalSession::returnToLobby() {
    if (m->state != SessionState::Running) return;
    m->state = SessionState::Lobby;
    m->localReady = false;
    m->rebuildMembers();
    MEALKIT_INFO("Back to the lobby after match %u", m->matchIndex);
}

bool LocalSession::canExecute(u32 tick) const {
    if (m->state != SessionState::Running) return false;
    if (m->replay) return tick < m->replayInputs.size();
    const Slot& s = m->slot(tick);
    return s.tick == tick && s.complete;
}

const TickInputs& LocalSession::inputs(u32 tick) const {
    if (m->replay) return m->replayInputs[tick];
    return m->slot(tick).inputs;
}

void LocalSession::tickExecuted(u32 tick, u64 stateHash) {
    if (m->replay) {
        if (tick < m->replayHashes.size() && m->replayHashes[tick] != stateHash && !m->desynced) {
            m->desynced = true;
            m->desyncTick = tick;
            MEALKIT_ERROR("Replay diverged at tick %u: state hash %016llx, recorded %016llx (different build or data?)", tick,
                          static_cast<unsigned long long>(stateHash), static_cast<unsigned long long>(m->replayHashes[tick]));
        }
        return;
    }
    forEachCommand(m->slot(tick).inputs, [&](u8, const u8* data, u32 size) { // engine command: input delay change
        u32 type = 0;
        if (size >= 5) std::memcpy(&type, data, 4);
        if (size == 5 && type == kDelayCommandType) m->applyDelay(tick, data[4]);
    });
}

void LocalSession::submitLocalInput(u32 tick, const PlayerInput& input) {
    if (m->state != SessionState::Running || m->replay) return;
    m->lastLocalInput = input;
    if (tick < m->finalNext) return; // already confirmed (the delay shrank)
    Slot& s = m->claim(tick);
    if (m->filled(tick)) return;
    s.inputs.players[0] = input;
    m->markFilled(tick);
    m->finalize();
}

bool LocalSession::submitCommand(const void* data, u32 size) {
    if (m->state != SessionState::Running || m->replay || size == 0 || size > kMaxCommandBytes) return false;
    if (m->pendingCommands.size() >= kMaxPendingCommands) return false;
    Command c;
    c.size = static_cast<u8>(size);
    std::memcpy(c.data, data, size);
    m->pendingCommands.push_back(c);
    return true;
}

u32 LocalSession::connectedMask() const { return (1u << m->playerCount) - 1u; }

} // namespace mealkit::session
