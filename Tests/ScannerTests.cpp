#include "Scanner.h"
#include <iostream>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
std::optional<std::string> Scan(Scanner::Assembler& a, const std::string& text, std::uint64_t start) {
    std::optional<std::string> out;
    for (char c : text) { out = a.Feed(c, start); start += 1000; }
    return out;
}
}
int main() try {
    Scanner::Assembler a;
    Check(!a.Feed('\n', 0), "Empty scans must be ignored");
    Check(Scan(a,"ABC123\n",1000).value() == "ABC123", "Scan assembly");
    Scanner::Assembler bounded({"SKU-", 5, 12, 300000});
    Check(!Scan(bounded,"OTHER\n",1000), "Prefix rejection");
    Check(Scan(bounded,"SKU-42\n",20000).value() == "SKU-42", "Prefix accepted");
    Check(!Scan(bounded,"SKU-123456789012345\n",40000), "Overlong frame rejected");
    Check(Scan(bounded,"SKU-7\n",80000).value() == "SKU-7", "Recover after overflow");
    a.Feed('X',200000); a.Feed('Y',600001);
    Check(a.Feed('\n',600002).value() == "Y", "Timeout discards partial frame");
    a.Feed('X',800000); a.Feed('Y',1);
    Check(a.Feed('\n',2).value() == "Y", "Clock rollback discards partial frame");
    Check(Scan(a,"AB\bC\n",900000).value() == "AC", "Backspace");
    Check(Scanner::CsvCell("=SUM(1,2)") == "\"'=SUM(1,2)\"", "Formula escape");
    Check(Scanner::CsvCell("A\"B") == "\"A\"\"B\"", "CSV quotes");
    Scanner::UsKeyboard keys;
    Check(keys.Feed(0x1E,0).value() == 'a', "US lower case");
    Check(!keys.Feed(0x1E,1), "Ignore break");
    keys.Feed(0x2A,0); Check(keys.Feed(0x1E,0).value() == 'A', "Shift");
    keys.Feed(0x2A,1); keys.Feed(0x3A,0);
    Check(keys.Feed(0x1E,0).value() == 'A', "Caps lock");
    keys.Feed(0x2A,0); Check(keys.Feed(0x1E,0).value() == 'a', "Shift caps");
    Check(!keys.Feed(0x1C,2), "Extended keys never treated as scanner text");
    keys.Reset();
    Scanner::KeyGate gate;
    Check(!gate.Block(1,"a",30,0,false),"Discovery forwards original down");
    Check(!gate.Block(1,"a",30,0,true),"Routing must forward repeats of an already forwarded hold");
    Check(!gate.Block(1,"a",30,1,true),"Forward the matching release after routing begins");
    Check(gate.Block(1,"a",30,0,true),"Suppress a new scanner hold");
    Check(gate.Block(1,"a",30,0,false),"A suppressed hold stays paired across route changes");
    Check(gate.Block(1,"a",30,1,false),"Suppress matching release of unseen original down");
    Check(!gate.Block(1,"a",30,0,false),"Next passthrough hold is forwarded");
    Check(gate.Block(1,"replacement",30,0,true),"Reused numeric ID never inherits old held keys");
    Check(!gate.Block(2,"other",30,0,false),"Other keyboards remain independent");
    Check(!gate.Block(1,"replacement",30,3,true),"Extended release is independent");
    for (unsigned i=0;i<100000;++i) {
        const auto text = "SKU-" + std::to_string(i % 10000) + "\n";
        const auto result = Scan(bounded,text,static_cast<std::uint64_t>(i)*100000);
        Check(result && *result == text.substr(0,text.size()-1), "Stress framing mismatch");
    }
    bool rejected = false;
    try { Scanner::Assembler invalid({"",0,4097,0}); } catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected,"Reject invalid configuration");
    std::cout << checks << " scanner checks passed. No driver, keyboard or network accessed.\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
