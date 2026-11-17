// Rollback netcode (in the manner of GGPO), the same code for the native
// client, the browser (Wasm) and the server's checks.
//
// The local player's inputs are delayed by a few frames; the remote player's
// inputs for frames not yet received are predicted (the last known input is
// repeated). When a remote input arrives that differs from the prediction, the
// game state saved at that frame is restored and the frames since are
// simulated again. The session stalls when the remote player falls further
// behind than the rollback window. Checksums of confirmed states (every
// input known) are published so that desynchronizations can be detected.
#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "game.hpp"

namespace arena {

struct SessionStats {
    int64_t frames = 0;           // frames advanced
    int64_t rollbacks = 0;        // times the state was restored
    int64_t resimulated = 0;      // frames simulated again
    int32_t max_rollback = 0;     // deepest rollback (frames)
    int64_t mispredicted = 0;     // remote inputs that differed from the prediction
    int64_t predicted = 0;        // remote inputs that had to be predicted
    int64_t stalls = 0;           // advance() refused: too far ahead of the remote player
    int64_t error_sum = 0;        // at each rollback: how far the players were from where they
    int32_t error_max = 0;        // should have been (fixed point, summed over both players)
};

class Session {
public:
    Session(Game& game, int local_player, int input_delay = 2, int window = 12, int checksum_every = 30);

    int frame() const { return frame_; }
    int local() const { return local_; }
    int input_delay() const { return delay_; }
    /// The local input for frame frame() + delay; returns that frame.
    int add_local_input(uint8_t input);
    /// A remote input (repeats are ignored); triggers a rollback at the next advance if it contradicts the prediction.
    void add_remote_input(int frame, uint8_t input);
    /// The first frame whose remote input is unknown.
    int remote_known() const { return int(known_[remote()].size()); }
    /// Frames with every input known, from 0 up to (not including) this one.
    int confirmed() const;
    bool can_advance() const { return frame_ - remote_known() < window_ && frame_ + delay_ <= int(known_[local_].size()); }
    /// Simulate one frame (rolling back first if needed); false (a stall) if it may not advance yet.
    bool advance();
    const State& state() const { return state_; }
    /// Local inputs from `from` on (to send, with redundancy).
    const std::vector<uint8_t>& local_inputs() const { return known_[local_]; }
    /// Checksums of newly confirmed frames (every checksum_every frames), taken once.
    std::vector<std::pair<int, uint64_t>> take_checksums();
    const SessionStats& stats() const { return stats_; }

private:
    Game& game_;
    int local_, delay_, window_, every_;
    int frame_ = 0;
    State state_;
    std::vector<uint8_t> known_[PLAYERS];  // inputs known, per player, from frame 0
    std::vector<uint8_t> used_;            // the remote input used for each simulated frame
    std::vector<uint8_t> was_predicted_;
    std::vector<State> ring_;              // state at the start of frame f in ring_[f % size]
    std::map<int, uint8_t> pending_;       // remote inputs that arrived ahead of a gap
    int rollback_from_ = -1;
    int next_checksum_ = 0;
    std::vector<std::pair<int, uint64_t>> checksums_;
    SessionStats stats_;

    int remote() const { return 1 - local_; }
    uint8_t remote_input_for(int f);
    void simulate(int f);
};

}  // namespace arena
