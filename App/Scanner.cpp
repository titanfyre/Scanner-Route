#include "Scanner.h"
#include <stdexcept>

namespace Scanner {
bool KeyGate::Block(unsigned device, const std::string& token, std::uint16_t scan,
                    std::uint16_t flags, bool suppress) {
    if (devices_.size() >= 4096 && !devices_.count(device))
        throw std::runtime_error("Too many keyboard identities; restart device detection.");
    auto& held = devices_[device];
    if (held.token != token) { held = {}; held.token = token; }
    const auto index = static_cast<std::size_t>(scan & 255u) + ((flags & 4) ? 512u : ((flags & 2) ? 256u : 0u));
    if (flags & 1) {
        const bool block = held.blocked.test(index);
        held.forwarded.reset(index); held.blocked.reset(index);
        return block;
    }
    if (held.blocked.test(index)) return true;
    if (suppress && !held.forwarded.test(index)) { held.blocked.set(index); return true; }
    held.forwarded.set(index);
    return false;
}
Assembler::Assembler(Rules rules) : rules_(std::move(rules)) {
    if (!rules_.minimum || rules_.maximum > 4096 || rules_.maximum < rules_.minimum ||
        rules_.prefix.size() > rules_.maximum || !rules_.gap_us || rules_.gap_us > 10000000)
        throw std::invalid_argument("Invalid scan length, prefix or inter-key timeout.");
    pending_.reserve(rules_.maximum);
}
void Assembler::Reset() noexcept { pending_.clear(); has_time_ = overflow_ = false; }
std::optional<std::string> Assembler::Feed(char c, std::uint64_t now) {
    if (has_time_ && (now < last_ || now - last_ > rules_.gap_us)) {
        if (!pending_.empty() || overflow_) ++rejected_;
        Reset();
    }
    last_ = now; has_time_ = true;
    if (c == '\r' || c == '\n') {
        if (overflow_ || pending_.size() < rules_.minimum ||
            pending_.compare(0, rules_.prefix.size(), rules_.prefix) != 0) {
            if (!pending_.empty() || overflow_) ++rejected_;
            Reset(); return {};
        }
        auto value = pending_;
        Reset(); return value;
    }
    if (c == '\b') { if (!overflow_ && !pending_.empty()) pending_.pop_back(); return {}; }
    if (c < 32 || c > 126 || overflow_) return {};
    if (pending_.size() == rules_.maximum) { pending_.clear(); overflow_ = true; return {}; }
    pending_ += c;
    return {};
}
std::optional<char> UsKeyboard::Feed(std::uint16_t scan, std::uint16_t flags) {
    const bool up = (flags & 1) != 0;
    if (flags & 6) return {};
    if (scan == 0x2A) { left_shift_ = !up; return {}; }
    if (scan == 0x36) { right_shift_ = !up; return {}; }
    if (up) return {};
    if (scan == 0x3A) { caps_ = !caps_; return {}; }
    if (scan == 0x1C) return '\n';
    if (scan == 0x0E) return '\b';
    if (scan == 0x39) return ' ';
    const bool shift = left_shift_ || right_shift_;
    if (scan >= 2 && scan <= 13) {
        constexpr char plain[] = "1234567890-=";
        constexpr char shifted[] = "!@#$%^&*()_+";
        return (shift ? shifted : plain)[scan - 2];
    }
    constexpr char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    char letter = 0;
    if (scan >= 0x10 && scan <= 0x19) letter = row1[scan-0x10];
    if (scan >= 0x1E && scan <= 0x26) letter = row2[scan-0x1E];
    if (scan >= 0x2C && scan <= 0x32) letter = row3[scan-0x2C];
    if (letter) return static_cast<char>((shift != caps_) ? letter-'a'+'A' : letter);
    switch (scan) {
    case 0x1A: return shift ? '{' : '['; case 0x1B: return shift ? '}' : ']';
    case 0x27: return shift ? ':' : ';'; case 0x28: return shift ? '"' : '\'';
    case 0x29: return shift ? '~' : '`'; case 0x2B: return shift ? '|' : '\\';
    case 0x33: return shift ? '<' : ','; case 0x34: return shift ? '>' : '.';
    case 0x35: return shift ? '?' : '/'; default: return {};
    }
}
std::string CsvCell(std::string_view text) {
    std::string out = "\"";
    if (!text.empty() && (text.front() == '=' || text.front() == '+' || text.front() == '-' ||
        text.front() == '@' || text.front() == '\t' || text.front() == '\r')) out += '\'';
    for (char c : text) { if (c == '"') out += '"'; out += c; }
    out += '"'; return out;
}
}
