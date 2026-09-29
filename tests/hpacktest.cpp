// hpacktest — feeds HPACK header blocks to the browser's decoder (browser/http2.cpp).
// Reads commands from stdin, one per line:
//   table N    start a new decoder whose table may hold N octets
//   hex HEX    decode a header block; prints "name: value" lines, then "--" (or "ERROR")
//   encode     then "name: value" lines and an empty line: encodes them, decodes the result
//              with a fresh decoder and prints it (a round trip)
// Run by tests/check_hpack.py.
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#include "../browser/http2.h"

int main() {
    auto dec = std::make_unique<h2::HpackDecoder>(4096);
    std::string line;
    auto print = [](bool ok, const h2::Headers &hs) {
        if (!ok) { std::cout << "ERROR\n"; return; }
        for (auto &h : hs) std::cout << h.first << ": " << h.second << "\n";
        std::cout << "--\n";
    };
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("table ", 0) == 0) {
            dec = std::make_unique<h2::HpackDecoder>((size_t)std::stoul(line.substr(6)));
        } else if (line.rfind("hex ", 0) == 0) {
            std::string hex = line.substr(4), bytes;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) bytes += (char)std::stoi(hex.substr(i, 2), nullptr, 16);
            h2::Headers hs;
            print(dec->decode((const uint8_t *)bytes.data(), bytes.size(), hs), hs);
        } else if (line == "encode") {
            h2::Headers in;
            while (std::getline(std::cin, line) && !line.empty() && line != "\r") {
                size_t c = line.find(": ", 1);
                in.push_back({line.substr(0, c), line.substr(c + 2)});
            }
            std::string block = h2::hpack_encode(in);
            h2::HpackDecoder fresh;
            h2::Headers out;
            print(fresh.decode((const uint8_t *)block.data(), block.size(), out), out);
        }
        std::cout.flush();
    }
}
