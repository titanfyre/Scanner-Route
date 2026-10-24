#pragma once
#include <cstdint>
#include <bitset>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Scanner {
// Keep releases paired with the decision made for the original key-down.
class KeyGate {
public:
    bool Block(unsigned device, const std::string& token, std::uint16_t scan,
               std::uint16_t flags, bool suppress);
private:
    struct Held { std::string token; std::bitset<768> forwarded, blocked; };
    std::unordered_map<unsigned, Held> devices_;
};
struct Rules {
    std::string prefix;
    std::size_t minimum = 1, maximum = 1024;
    std::uint64_t gap_us = 300000;
};
class Assembler {
public:
    explicit Assembler(Rules rules = {});
    std::optional<std::string> Feed(char character, std::uint64_t now_us);
    void Reset() noexcept;
    std::uint64_t Rejected() const noexcept { return rejected_; }
private:
    Rules rules_;
    std::string pending_;
    std::uint64_t last_ = 0, rejected_ = 0;
    bool has_time_ = false, overflow_ = false;
};
class UsKeyboard {
public:
    std::optional<char> Feed(std::uint16_t scan, std::uint16_t flags);
    void Reset() noexcept { left_shift_ = right_shift_ = caps_ = false; }
private:
    bool left_shift_ = false, right_shift_ = false, caps_ = false;
};
std::string CsvCell(std::string_view text);
}
