#include "arena/session.hpp"

#include <algorithm>
#include <stdexcept>

namespace arena {

Session::Session(Game& game, int local_player, int input_delay, int window, int checksum_every)
    : game_(game), local_(local_player), delay_(input_delay), window_(window), every_(checksum_every) {
    if (local_ < 0 || local_ >= PLAYERS) throw std::invalid_argument("local player");
    state_ = game_.initial();
    ring_.resize(size_t(window_) + 2);
    // the first `delay` frames have no input from anybody (both sides agree on it)
    for (auto& k : known_) k.assign(size_t(delay_), 0);
}

int Session::add_local_input(uint8_t input) {
    known_[local_].push_back(input);
    return int(known_[local_].size()) - 1;
}

void Session::add_remote_input(int f, uint8_t input) {
    auto& k = known_[remote()];
    if (f < int(k.size())) return;  // a repeat (inputs are sent redundantly)
    if (f > int(k.size())) {
        // a gap: wait for the missing frames (they come again with the next packets)
        pending_.emplace(f, input);
        return;
    }
    k.push_back(input);
    // fill from the frames that arrived early
    for (auto it = pending_.find(int(k.size())); it != pending_.end(); it = pending_.find(int(k.size()))) {
        k.push_back(it->second);
        pending_.erase(it);
    }
    while (!pending_.empty() && pending_.begin()->first < int(k.size())) pending_.erase(pending_.begin());
    // contradictions with what was simulated: roll back to the earliest
    for (int g = f; g < int(k.size()) && g < frame_; ++g) {
        if (was_predicted_[g] && used_[g] != k[g]) {
            if (rollback_from_ < 0 || g < rollback_from_) rollback_from_ = g;
            ++stats_.mispredicted;
            break;
        }
    }
}

int Session::confirmed() const {
    int c = std::min(int(known_[0].size()), int(known_[1].size()));
    c = std::min(c, frame_);
    if (rollback_from_ >= 0) c = std::min(c, rollback_from_);
    return c;
}

uint8_t Session::remote_input_for(int f) {
    const auto& k = known_[remote()];
    if (f < int(k.size())) return k[f];
    return k.empty() ? 0 : k.back();  // predict: the last known input again
}

void Session::simulate(int f) {
    ring_[size_t(f) % ring_.size()] = state_;
    uint8_t in[PLAYERS];
    in[local_] = known_[local_][size_t(f)];
    in[remote()] = remote_input_for(f);
    if (int(used_.size()) <= f) {
        used_.resize(size_t(f) + 1);
        was_predicted_.resize(size_t(f) + 1);
    }
    bool predicted = f >= remote_known();
    if (predicted && !was_predicted_[f]) ++stats_.predicted;
    used_[f] = in[remote()];
    was_predicted_[f] = predicted;
    game_.step(state_, in);
}

bool Session::advance() {
    if (!can_advance()) {
        ++stats_.stalls;
        return false;
    }
    if (rollback_from_ >= 0) {
        int from = rollback_from_;
        rollback_from_ = -1;
        if (frame_ - from > int(ring_.size()) - 1) throw std::logic_error("rollback beyond the saved states");
        const State shown = state_;  // what the player was seeing
        state_ = ring_[size_t(from) % ring_.size()];
        ++stats_.rollbacks;
        stats_.max_rollback = std::max(stats_.max_rollback, frame_ - from);
        for (int f = from; f < frame_; ++f) {
            simulate(f);
            ++stats_.resimulated;
        }
        fx err = 0;
        for (int i = 0; i < PLAYERS; ++i) err += fx_len(state_.p[i].x - shown.p[i].x, state_.p[i].y - shown.p[i].y);
        stats_.error_sum += err;
        stats_.error_max = std::max(stats_.error_max, err);
    }
    simulate(frame_);
    ++frame_;
    ++stats_.frames;
    // checksums of states that can no longer change
    int c = confirmed();
    while (next_checksum_ <= c && next_checksum_ >= frame_ - int(ring_.size()) + 1) {
        const State& s = next_checksum_ == frame_ ? state_ : ring_[size_t(next_checksum_) % ring_.size()];
        checksums_.emplace_back(next_checksum_, Game::hash(s));
        next_checksum_ += every_;
    }
    return true;
}

std::vector<std::pair<int, uint64_t>> Session::take_checksums() {
    auto out = std::move(checksums_);
    checksums_.clear();
    return out;
}

}  // namespace arena
