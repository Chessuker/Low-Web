// http2.h — HTTP/2 (RFC 9113) for net.cpp: many requests at once over one TLS connection,
// with HPACK header compression (RFC 7541). A connection has a reader thread that sorts the
// server's frames into streams; each request waits on its own stream.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "net.h"

namespace net {
struct Conn;
}

namespace h2 {

using Headers = std::vector<std::pair<std::string, std::string>>;

// ---- HPACK --------------------------------------------------------------------------------

class HpackDecoder {
public:
    explicit HpackDecoder(size_t max_table = 4096) : max_(max_table), limit_(max_table) {}
    // Decodes one header block; false if it is malformed (a connection error).
    bool decode(const uint8_t *p, size_t n, Headers &out);

private:
    bool entry(size_t index, std::pair<std::string, std::string> &out) const;
    void add(const std::string &name, const std::string &value);
    std::deque<std::pair<std::string, std::string>> table_;  // front = newest
    size_t size_ = 0, max_, limit_;                            // limit_: what our SETTINGS allow
};

// Encodes headers as literals that the server doesn't index (no shared state to keep).
std::string hpack_encode(const Headers &headers);
bool huffman_decode(const uint8_t *p, size_t n, std::string &out);

// ---- streams and connections ----------------------------------------------------------------

struct Stream {
    std::mutex m;
    std::condition_variable cv;
    uint32_t id = 0;
    bool got_headers = false, ended = false, failed = false;
    bool retry_ok = false;  // failed before the server looked at it: safe to send again
    int status = 0;
    Headers headers;
    std::deque<std::vector<uint8_t>> data;
    std::string error;
    int64_t send_window = 65535;

    // For the requesting thread. Wait for the answer's headers; false on failure.
    bool wait_headers(int timeout_ms);
    // Next piece of the body: 1 = got one, 0 = the body is complete, -1 = failed.
    int next_data(std::vector<uint8_t> &out, int timeout_ms);
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(std::unique_ptr<net::Conn> conn, std::string key);
    ~Connection();
    // Sends the connection preface and starts the reader thread. False if the server is gone.
    bool start();
    // Opens a stream with these headers (and a body, for POST). Null if this connection can't
    // take more (closing, broken): open another one.
    std::shared_ptr<Stream> request(const Headers &headers, const std::string *body);
    bool usable();
    const std::string &key() const { return key_; }
    net::Zone zone() const;  // of the server's address

private:
    void reader();
    bool write_frame(uint8_t type, uint8_t flags, uint32_t stream, const uint8_t *payload, size_t n);
    bool write_frame(uint8_t type, uint8_t flags, uint32_t stream, const std::string &payload) {
        return write_frame(type, flags, stream, (const uint8_t *)payload.data(), payload.size());
    }
    void fail_all(const std::string &why, uint32_t retry_above);
    void on_headers(uint32_t id, const std::string &block, bool end_stream);

    std::unique_ptr<net::Conn> c_;
    std::string key_;
    std::mutex m_;             // everything below
    std::mutex wm_;            // writing to the socket (frames must not interleave)
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Stream>> streams_;
    uint32_t next_id_ = 1;
    bool dead_ = false, going_away_ = false;
    uint32_t peer_max_streams_ = 100, peer_max_frame_ = 16384;
    int64_t peer_initial_window_ = 65535, send_window_ = 65535;
    HpackDecoder hpack_;  // used by the reader thread only
};

// A live HTTP/2 connection to this origin ("https://host:port") that can take a request, or null.
std::shared_ptr<Connection> find(const std::string &key);
// Makes a connection whose TLS handshake agreed on "h2" into an HTTP/2 connection.
std::shared_ptr<Connection> adopt(std::unique_ptr<net::Conn> conn, const std::string &key);

}  // namespace h2
