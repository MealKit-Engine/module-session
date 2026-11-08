#pragma once

#include "mealkit/session/Session.h"

#include <memory>

// A session without a network (docs/ENGINE_MODULES.md "Multiplayer as a module"): solo play and replays.
// Builds without the net module play through this alone; with net, game::MatchService uses it for playOffline/playReplay too.
// Ticks follow net::LockstepSession's host path exactly (input delay 1, the first delay ticks confirmed empty, commands packed into the tick
// my input confirms, the engine input delay command), so a match recorded here replays the same with either one.
namespace mealkit::session {

class LocalSession final : public Session {
public:
    LocalSession();
    ~LocalSession() override;
    LocalSession(const LocalSession&) = delete;
    LocalSession& operator=(const LocalSession&) = delete;

    // Solo: startOffline goes straight to the match, openOffline starts at the lobby (start with startMatch). Only from Idle
    bool startOffline(u64 seed, u32 matchData);
    bool openOffline();
    // Reruns a recorded match (only from Idle). Running at once, canExecute only up to the recorded ticks, my inputs/commands discarded.
    // With hashes, tickExecuted compares them and reports the first differing tick as a desync
    bool startReplay(u64 seed, u32 matchData, const std::vector<std::vector<u8>>& loadouts, std::vector<TickInputs> inputs,
                     std::vector<u64> hashes);
    // Mid-match: changes input delay from the same tick, through the same engine command as net (tests, game settings)
    bool requestInputDelay(u32 ticks);
    void setContentHash(u64 hash);
    void setCrossplay(bool allowed);
    // Call every frame (chat pacing, lobby list)
    void update(f64 now);
    // Back to Idle. Content hash, name, loadout, input kind and crossplay remain
    void reset();

    SessionState state() const override;
    const char* endReason() const override;
    const RejectInfo& rejectInfo() const override;
    bool isHost() const override;
    bool isOffline() const override;
    bool isReplay() const override;
    u32 replayTicks() const override;
    u8 localPlayer() const override;
    u32 playerCount() const override;
    u32 lobbyPlayerCount() const override;
    u32 maxPlayers() const override;
    u64 seed() const override;
    u32 matchData() const override;
    u32 matchIndex() const override;
    u32 inputDelay() const override;
    u64 contentHash() const override;
    bool crossplay() const override;
    u32 hostMigrations() const override;
    const SessionStats& stats() const override;
    u32 pingMs(u32 player) const override;
    u32 waitingFor() const override;
    bool desynced() const override;
    u32 desyncTick() const override;

    void setLocalName(const std::string& name) override;
    const std::string& localName() const override;
    void setLocalReady(bool ready) override;
    bool localReady() const override;
    void setLocalInputKind(InputKind kind) override;
    InputKind localInputKind() const override;
    const std::vector<LobbyMember>& lobbyMembers() const override;
    bool setLocalLoadout(const void* data, u32 size) override;
    const std::vector<u8>& loadout(u32 player) const override;
    bool sendChat(const std::string& text) override;
    const std::vector<ChatMessage>& chatLog() const override;
    void startMatch(u64 seed, u32 matchData) override;
    void returnToLobby() override;

    bool canExecute(u32 tick) const override;
    const TickInputs& inputs(u32 tick) const override;
    void tickExecuted(u32 tick, u64 stateHash) override;
    void submitLocalInput(u32 tick, const PlayerInput& input) override;
    bool submitCommand(const void* data, u32 size) override;
    u32 connectedMask() const override;

    // No late join without a network
    bool snapshotWanted() const override { return false; }
    void provideSnapshot(u32, const std::vector<u8>&) override {}
    u32 joinTick() const override { return 0; }
    bool takeJoinSnapshot(std::vector<u8>&) override { return false; }
    bool catchingUp() const override { return false; }

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Forwards every call to the session set with setTarget. game::MatchService hands this to game code (GameContext::session), so the
// reference stays the same while the match moves between a local session and a network one
class SessionSwitch final : public Session {
public:
    explicit SessionSwitch(Session& target) : t(&target) {}
    void setTarget(Session& target) { t = &target; }
    Session& target() const { return *t; }

    SessionState state() const override { return t->state(); }
    const char* endReason() const override { return t->endReason(); }
    const RejectInfo& rejectInfo() const override { return t->rejectInfo(); }
    bool isHost() const override { return t->isHost(); }
    bool isOffline() const override { return t->isOffline(); }
    bool isReplay() const override { return t->isReplay(); }
    u32 replayTicks() const override { return t->replayTicks(); }
    u8 localPlayer() const override { return t->localPlayer(); }
    u32 playerCount() const override { return t->playerCount(); }
    u32 lobbyPlayerCount() const override { return t->lobbyPlayerCount(); }
    u32 maxPlayers() const override { return t->maxPlayers(); }
    u64 seed() const override { return t->seed(); }
    u32 matchData() const override { return t->matchData(); }
    u32 matchIndex() const override { return t->matchIndex(); }
    u32 inputDelay() const override { return t->inputDelay(); }
    u64 contentHash() const override { return t->contentHash(); }
    bool crossplay() const override { return t->crossplay(); }
    u32 hostMigrations() const override { return t->hostMigrations(); }
    const SessionStats& stats() const override { return t->stats(); }
    u32 pingMs(u32 player) const override { return t->pingMs(player); }
    u32 waitingFor() const override { return t->waitingFor(); }
    bool desynced() const override { return t->desynced(); }
    u32 desyncTick() const override { return t->desyncTick(); }
    void setLocalName(const std::string& name) override { t->setLocalName(name); }
    const std::string& localName() const override { return t->localName(); }
    void setLocalReady(bool ready) override { t->setLocalReady(ready); }
    bool localReady() const override { return t->localReady(); }
    void setLocalInputKind(InputKind kind) override { t->setLocalInputKind(kind); }
    InputKind localInputKind() const override { return t->localInputKind(); }
    const std::vector<LobbyMember>& lobbyMembers() const override { return t->lobbyMembers(); }
    bool setLocalLoadout(const void* data, u32 size) override { return t->setLocalLoadout(data, size); }
    const std::vector<u8>& loadout(u32 player) const override { return t->loadout(player); }
    bool sendChat(const std::string& text) override { return t->sendChat(text); }
    const std::vector<ChatMessage>& chatLog() const override { return t->chatLog(); }
    void startMatch(u64 seed, u32 matchData) override { t->startMatch(seed, matchData); }
    void returnToLobby() override { t->returnToLobby(); }
    bool canExecute(u32 tick) const override { return t->canExecute(tick); }
    const TickInputs& inputs(u32 tick) const override { return t->inputs(tick); }
    void tickExecuted(u32 tick, u64 stateHash) override { t->tickExecuted(tick, stateHash); }
    void submitLocalInput(u32 tick, const PlayerInput& input) override { t->submitLocalInput(tick, input); }
    bool submitCommand(const void* data, u32 size) override { return t->submitCommand(data, size); }
    u32 connectedMask() const override { return t->connectedMask(); }
    bool snapshotWanted() const override { return t->snapshotWanted(); }
    void provideSnapshot(u32 tick, const std::vector<u8>& bytes) override { t->provideSnapshot(tick, bytes); }
    u32 joinTick() const override { return t->joinTick(); }
    bool takeJoinSnapshot(std::vector<u8>& out) override { return t->takeJoinSnapshot(out); }
    bool catchingUp() const override { return t->catchingUp(); }

private:
    Session* t;
};

} // namespace mealkit::session
