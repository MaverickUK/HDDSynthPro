#include "media_watch.h"

namespace media_watch {

const char* stateName(State s) {
    switch (s) {
        case State::None: return "none";
        case State::Present: return "present";
        case State::Lost: return "lost";
        case State::Changed: return "changed";
    }
    return "?";
}

const char* difference(const Identity& served, const Identity& now) {
    if (now.maker != served.maker || now.serial != served.serial) return "another card";
    if (now.size != served.size || now.first_sector != served.first_sector) return "image replaced";
    if (now.date != served.date || now.time != served.time) return "image modified";
    return nullptr;
}

void Watch::serve(const Identity& id) {
    served_ = id;
    state_ = State::Present;
    why_ = "";
}

bool Watch::failed(uint32_t now_ms) {
    if (state_ != State::Present) return false;
    state_ = State::Lost;
    last_try_ms_ = now_ms;
    ++losses_;
    return true;
}

bool Watch::due(uint32_t now_ms) {
    uint32_t every;
    if (state_ == State::Lost) every = PROBE_MS;
    else if (state_ == State::Changed) every = CHANGED_CHECK_MS;
    else return false;
    if (now_ms - last_try_ms_ < every) return false;
    last_try_ms_ = now_ms;
    return true;
}

State Watch::cameBack(const Identity& id, uint32_t now_ms) {
    if (state_ != State::Lost && state_ != State::Changed) return state_;
    last_try_ms_ = now_ms;
    const char* diff = difference(served_, id);
    if (diff) {
        state_ = State::Changed;
        why_ = diff;
    } else {
        state_ = State::Present;
        why_ = "";
        ++returns_;
    }
    return state_;
}

void Watch::cameBackWithout(const char* why, uint32_t now_ms) {
    if (state_ != State::Lost && state_ != State::Changed) return;
    state_ = State::Changed;
    why_ = why;
    last_try_ms_ = now_ms;
}

void Watch::gone(uint32_t now_ms) {
    if (state_ != State::Changed) return;
    state_ = State::Lost;
    why_ = "";
    last_try_ms_ = now_ms;
}

}  // namespace media_watch
