// http2.cpp — see http2.h.
#include "http2.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "conn.h"
#include "hpack_huffman.h"

namespace h2 {

// =====================================================================================
// HPACK (RFC 7541)
// =====================================================================================

namespace {

const char *const kStatic[61][2] = {
    {":authority", ""}, {":method", "GET"}, {":method", "POST"}, {":path", "/"}, {":path", "/index.html"},
    {":scheme", "http"}, {":scheme", "https"}, {":status", "200"}, {":status", "204"}, {":status", "206"},
    {":status", "304"}, {":status", "400"}, {":status", "404"}, {":status", "500"}, {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"}, {"accept-language", ""}, {"accept-ranges", ""}, {"accept", ""},
    {"access-control-allow-origin", ""}, {"age", ""}, {"allow", ""}, {"authorization", ""}, {"cache-control", ""},
    {"content-disposition", ""}, {"content-encoding", ""}, {"content-language", ""}, {"content-length", ""},
    {"content-location", ""}, {"content-range", ""}, {"content-type", ""}, {"cookie", ""}, {"date", ""},
    {"etag", ""}, {"expect", ""}, {"expires", ""}, {"from", ""}, {"host", ""}, {"if-match", ""},
    {"if-modified-since", ""}, {"if-none-match", ""}, {"if-range", ""}, {"if-unmodified-since", ""},
    {"last-modified", ""}, {"link", ""}, {"location", ""}, {"max-forwards", ""}, {"proxy-authenticate", ""},
    {"proxy-authorization", ""}, {"range", ""}, {"referer", ""}, {"refresh", ""}, {"retry-after", ""},
    {"server", ""}, {"set-cookie", ""}, {"strict-transport-security", ""}, {"transfer-encoding", ""},
    {"user-agent", ""}, {"vary", ""}, {"via", ""}, {"www-authenticate", ""},
};

// Huffman decoding tree: node 0 is the root; child[bit] < 0 means a leaf for symbol ~child.
struct HuffTree {
    std::vector<int> child[2];
    HuffTree() {
        child[0].push_back(0);
        child[1].push_back(0);
        for (int sym = 0; sym < 257; sym++) {
            int node = 0;
            for (int b = kHuffman[sym].bits - 1; b >= 0; b--) {
                int bit = (kHuffman[sym].code >> b) & 1;
                if (b == 0) {
                    child[bit][node] = ~sym;
                } else {
                    if (child[bit][node] <= 0) {
                        child[bit][node] = (int)child[0].size();
                        child[0].push_back(0);
                        child[1].push_back(0);
                    }
                    node = child[bit][node];
                }
            }
        }
    }
};

bool read_int(const uint8_t *&p, const uint8_t *e, int prefix, uint64_t &v) {
    if (p >= e) return false;
    uint64_t mask = (1u << prefix) - 1;
    v = *p++ & mask;
    if (v < mask) return true;
    for (int shift = 0; p < e && shift <= 28; shift += 7) {
        uint8_t b = *p++;
        v += (uint64_t)(b & 127) << shift;
        if (!(b & 128)) return true;
    }
    return false;
}

bool read_string(const uint8_t *&p, const uint8_t *e, std::string &out) {
    if (p >= e) return false;
    bool huff = *p & 0x80;
    uint64_t len;
    if (!read_int(p, e, 7, len) || len > (uint64_t)(e - p)) return false;
    bool ok = huff ? huffman_decode(p, (size_t)len, out) : (out.assign((const char *)p, (size_t)len), true);
    p += len;
    return ok;
}

void put_int(std::string &out, uint8_t first, int prefix, uint64_t v) {
    uint64_t mask = (1u << prefix) - 1;
    if (v < mask) {
        out += (char)(first | v);
        return;
    }
    out += (char)(first | mask);
    v -= mask;
    while (v >= 128) {
        out += (char)(0x80 | (v & 127));
        v >>= 7;
    }
    out += (char)v;
}

}  // namespace

bool huffman_decode(const uint8_t *p, size_t n, std::string &out) {
    static const HuffTree tree;
    out.clear();
    int node = 0, depth = 0;
    bool all_ones = true;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            int bit = (p[i] >> b) & 1;
            int next = tree.child[bit][node];
            depth++;
            all_ones &= bit == 1;
            if (next < 0) {
                int sym = ~next;
                if (sym == 256) return false;  // EOS inside a string
                out += (char)sym;
                node = depth = 0;
                all_ones = true;
            } else if (next == 0) {
                return false;
            } else {
                node = next;
            }
        }
    }
    return depth < 8 && all_ones;  // padding: fewer than 8 bits, all ones (a prefix of EOS)
}

bool HpackDecoder::entry(size_t index, std::pair<std::string, std::string> &out) const {
    if (index >= 1 && index <= 61) {
        out = {kStatic[index - 1][0], kStatic[index - 1][1]};
        return true;
    }
    if (index >= 62 && index - 62 < table_.size()) {
        out = table_[index - 62];
        return true;
    }
    return false;
}

void HpackDecoder::add(const std::string &name, const std::string &value) {
    size_t sz = name.size() + value.size() + 32;
    while (!table_.empty() && size_ + sz > max_) {
        size_ -= table_.back().first.size() + table_.back().second.size() + 32;
        table_.pop_back();
    }
    if (sz > max_) return;  // bigger than the whole table: it just empties it
    table_.emplace_front(name, value);
    size_ += sz;
}

bool HpackDecoder::decode(const uint8_t *p, size_t n, Headers &out) {
    const uint8_t *e = p + n;
    uint64_t v;
    while (p < e) {
        uint8_t b = *p;
        std::pair<std::string, std::string> h;
        if (b & 0x80) {  // indexed
            if (!read_int(p, e, 7, v) || !entry((size_t)v, h)) return false;
            out.push_back(h);
        } else if ((b & 0xC0) == 0x40 || (b & 0xE0) == 0 || (b & 0xF0) == 0x10) {  // literals
            bool index = (b & 0xC0) == 0x40;
            if (!read_int(p, e, index ? 6 : 4, v)) return false;
            if (v) {
                if (!entry((size_t)v, h)) return false;
            } else if (!read_string(p, e, h.first)) {
                return false;
            }
            if (!read_string(p, e, h.second)) return false;
            if (index) add(h.first, h.second);
            out.push_back(std::move(h));
        } else {  // 001xxxxx: dynamic table size update
            if (!read_int(p, e, 5, v) || v > limit_) return false;
            max_ = (size_t)v;
            while (!table_.empty() && size_ > max_) {
                size_ -= table_.back().first.size() + table_.back().second.size() + 32;
                table_.pop_back();
            }
        }
    }
    return true;
}

std::string hpack_encode(const Headers &headers) {
    std::string out;
    for (const auto &h : headers) {  // "literal header field without indexing - new name"
        out += '\0';
        put_int(out, 0, 7, h.first.size());
        out += h.first;
        put_int(out, 0, 7, h.second.size());
        out += h.second;
    }
    return out;
}

// =====================================================================================
// streams
// =====================================================================================

bool Stream::wait_headers(int timeout_ms) {
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return got_headers || failed; });
    if (!got_headers && !failed) {
        failed = true;
        error = "the server did not answer";
    }
    return got_headers;
}

int Stream::next_data(std::vector<uint8_t> &out, int timeout_ms) {
    std::unique_lock<std::mutex> lk(m);
    if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !data.empty() || ended || failed; })) {
        failed = true;
        error = "the server stopped sending";
        return -1;
    }
    if (!data.empty()) {
        out = std::move(data.front());
        data.pop_front();
        return 1;
    }
    return failed ? -1 : 0;
}

// =====================================================================================
// connections
// =====================================================================================

namespace {

enum : uint8_t { DATA = 0, HEADERS = 1, PRIORITY = 2, RST_STREAM = 3, SETTINGS = 4, PUSH_PROMISE = 5, PING = 6,
                 GOAWAY = 7, WINDOW_UPDATE = 8, CONTINUATION = 9 };
enum : uint8_t { END_STREAM = 1, ACK = 1, END_HEADERS = 4, PADDED = 8, PRIORITY_FLAG = 0x20 };
const uint32_t kOurWindow = 16u << 20;  // we take up to 16 MB before the server has to wait
const int64_t kAckEvery = 4 << 20;      // and tell it we took data every 4 MB

std::mutex g_reg_m;
std::vector<std::shared_ptr<Connection>> g_conns;

uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string be32(uint32_t v) {
    return std::string{(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
}

}  // namespace

Connection::Connection(std::unique_ptr<net::Conn> conn, std::string key) : c_(std::move(conn)), key_(std::move(key)) {}
Connection::~Connection() = default;

bool Connection::write_frame(uint8_t type, uint8_t flags, uint32_t stream, const uint8_t *payload, size_t n) {
    std::string f;
    f.reserve(9 + n);
    f += (char)(n >> 16);
    f += (char)(n >> 8);
    f += (char)n;
    f += (char)type;
    f += (char)flags;
    f += be32(stream & 0x7fffffff);
    f.append((const char *)payload, n);
    if (c_->send_all(f)) return true;
    std::lock_guard<std::mutex> lk(m_);
    dead_ = true;
    return false;
}

bool Connection::start() {
    DWORD idle = 60000;  // an idle connection is closed after a minute (see reader)
    setsockopt(c_->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&idle, sizeof idle);
    std::string settings;
    auto setting = [&](uint16_t id, uint32_t v) { settings += (char)(id >> 8); settings += (char)id; settings += be32(v); };
    setting(2, 0);           // ENABLE_PUSH: no
    setting(4, kOurWindow);  // INITIAL_WINDOW_SIZE
    std::lock_guard<std::mutex> w(wm_);
    if (!c_->send_all("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n")) return false;
    if (!write_frame(SETTINGS, 0, 0, settings)) return false;
    if (!write_frame(WINDOW_UPDATE, 0, 0, be32(kOurWindow - 65535))) return false;  // the connection's window too
    std::thread([self = shared_from_this()] { self->reader(); }).detach();
    return true;
}

net::Zone Connection::zone() const { return c_->zone; }

bool Connection::usable() {
    std::lock_guard<std::mutex> lk(m_);
    return !dead_ && !going_away_;
}

std::shared_ptr<Stream> Connection::request(const Headers &headers, const std::string *body) {
    auto st = std::make_shared<Stream>();
    {
        std::unique_lock<std::mutex> lk(m_);  // wait for a free stream slot
        cv_.wait_for(lk, std::chrono::seconds(30), [&] { return dead_ || going_away_ || streams_.size() < peer_max_streams_; });
        if (dead_ || going_away_ || streams_.size() >= peer_max_streams_) return nullptr;
    }
    std::string block = hpack_encode(headers);
    bool has_body = body && !body->empty();
    {
        std::lock_guard<std::mutex> w(wm_);  // ids must go out in increasing order
        size_t max_frame;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (dead_ || going_away_) return nullptr;
            st->id = next_id_;
            next_id_ += 2;
            st->send_window = peer_initial_window_;
            streams_.push_back(st);
            max_frame = peer_max_frame_;
        }
        size_t first = std::min(block.size(), max_frame);
        bool ok = write_frame(HEADERS, (has_body ? 0 : END_STREAM) | (first == block.size() ? END_HEADERS : 0), st->id,
                              (const uint8_t *)block.data(), first);
        for (size_t at = first; ok && at < block.size();) {
            size_t n = std::min(block.size() - at, max_frame);
            ok = write_frame(CONTINUATION, at + n == block.size() ? END_HEADERS : 0, st->id, (const uint8_t *)block.data() + at, n);
            at += n;
        }
        if (!ok) return nullptr;
    }
    // the body, within the flow-control windows the server gives us
    for (size_t at = 0; has_body && at < body->size();) {
        size_t n;
        {
            std::unique_lock<std::mutex> lk(m_);
            if (!cv_.wait_for(lk, std::chrono::seconds(30), [&] { return dead_ || (send_window_ > 0 && st->send_window > 0); }) || dead_)
                return nullptr;
            n = (size_t)std::min<int64_t>({(int64_t)(body->size() - at), send_window_, st->send_window, (int64_t)peer_max_frame_});
            send_window_ -= n;
            st->send_window -= n;
        }
        std::lock_guard<std::mutex> w(wm_);
        if (!write_frame(DATA, at + n == body->size() ? END_STREAM : 0, st->id, (const uint8_t *)body->data() + at, n)) return nullptr;
        at += n;
    }
    return st;
}

void Connection::cancel(const std::shared_ptr<Stream> &st) {
    bool open;
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = std::find(streams_.begin(), streams_.end(), st);
        open = it != streams_.end();
        if (open) streams_.erase(it);
        cv_.notify_all();
    }
    if (!open) return;  // already ended
    std::lock_guard<std::mutex> w(wm_);
    write_frame(RST_STREAM, 0, st->id, be32(8));  // CANCEL
}

void Connection::fail_all(const std::string &why, uint32_t retry_above) {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = streams_.begin(); it != streams_.end();) {
        Stream &s = **it;
        if (s.id <= retry_above) { ++it; continue; }
        {
            std::lock_guard<std::mutex> sl(s.m);
            if (!s.ended) {
                s.failed = true;
                s.error = why;
                s.retry_ok = !s.got_headers;  // nothing came back: the request can go out again
            }
            s.cv.notify_all();
        }
        it = streams_.erase(it);
    }
    cv_.notify_all();
}

void Connection::on_headers(uint32_t id, const std::string &block, bool end_stream) {
    Headers hs;
    if (!hpack_.decode((const uint8_t *)block.data(), block.size(), hs)) throw std::string("bad header compression");
    std::shared_ptr<Stream> st;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto &s : streams_)
            if (s->id == id) st = s;
    }
    if (!st) return;  // one we gave up on
    int status = 0;
    for (auto &h : hs)
        if (h.first == ":status") status = atoi(h.second.c_str());
    bool finished = false;
    {
        std::lock_guard<std::mutex> sl(st->m);
        if (!st->got_headers) {
            if (status >= 100 && status < 200) return;  // "103 Early Hints" and such: the real answer follows
            st->got_headers = true;
            st->status = status;
            for (auto &h : hs)
                if (!h.first.empty() && h.first[0] != ':') st->headers.push_back(std::move(h));
        }  // else: trailers, which we don't need
        if (end_stream) finished = st->ended = true;
        st->cv.notify_all();
    }
    if (finished) {
        std::lock_guard<std::mutex> lk(m_);
        streams_.erase(std::remove(streams_.begin(), streams_.end(), st), streams_.end());
        cv_.notify_all();
    }
}

void Connection::reader() {
    std::vector<uint8_t> buf;
    size_t off = 0;
    std::vector<char> tmp(65536);
    auto need = [&](size_t n) {
        while (buf.size() - off < n) {
            if (off > (1u << 20)) {
                buf.erase(buf.begin(), buf.begin() + off);
                off = 0;
            }
            int k = c_->read(tmp.data(), (int)tmp.size());
            if (k <= 0) return false;
            buf.insert(buf.end(), tmp.data(), tmp.data() + k);
        }
        return true;
    };
    std::string why = "the server closed the connection";
    std::string block;
    uint32_t block_stream = 0;
    bool in_block = false, block_end_stream = false;
    int64_t conn_taken = 0;
    std::vector<std::pair<uint32_t, int64_t>> taken;  // per stream, not yet acknowledged
    try {
        for (;;) {
            if (!need(9)) break;
            const uint8_t *h = &buf[off];
            uint32_t len = (uint32_t)h[0] << 16 | (uint32_t)h[1] << 8 | h[2];
            uint8_t type = h[3], flags = h[4];
            uint32_t sid = u32(h + 5) & 0x7fffffff;
            if (len > (1u << 24) - 1 || !need(9 + len)) break;
            const uint8_t *p = &buf[off + 9];
            off += 9 + len;
            if (in_block && type != CONTINUATION) throw std::string("header block interrupted");
            switch (type) {
            case DATA: {
                if (sid == 0) throw std::string("DATA on stream 0");
                size_t n = len, pad = 0;
                const uint8_t *d = p;
                if (flags & PADDED) {
                    if (!n) throw std::string("bad padding");
                    pad = *d++;
                    n--;
                    if (pad > n) throw std::string("bad padding");
                    n -= pad;
                }
                std::shared_ptr<Stream> st;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    for (auto &s : streams_)
                        if (s->id == sid) st = s;
                    if (st && (flags & END_STREAM)) {
                        streams_.erase(std::remove(streams_.begin(), streams_.end(), st), streams_.end());
                        cv_.notify_all();
                    }
                }
                if (st) {
                    std::lock_guard<std::mutex> sl(st->m);
                    if (n) st->data.emplace_back(d, d + n);
                    if (flags & END_STREAM) st->ended = true;
                    st->cv.notify_all();
                }
                // flow control: give the window back as we take data
                conn_taken += len;
                if (conn_taken >= kAckEvery) {
                    std::lock_guard<std::mutex> w(wm_);
                    write_frame(WINDOW_UPDATE, 0, 0, be32((uint32_t)conn_taken));
                    conn_taken = 0;
                }
                if (st && !(flags & END_STREAM)) {
                    auto it = std::find_if(taken.begin(), taken.end(), [&](auto &t) { return t.first == sid; });
                    if (it == taken.end()) it = taken.insert(taken.end(), {sid, 0});
                    it->second += len;
                    if (it->second >= kAckEvery) {
                        std::lock_guard<std::mutex> w(wm_);
                        write_frame(WINDOW_UPDATE, 0, sid, be32((uint32_t)it->second));
                        it->second = 0;
                    }
                } else {
                    taken.erase(std::remove_if(taken.begin(), taken.end(), [&](auto &t) { return t.first == sid; }), taken.end());
                }
                break;
            }
            case HEADERS: {
                if (sid == 0) throw std::string("HEADERS on stream 0");
                size_t n = len, pad = 0;
                const uint8_t *d = p;
                if (flags & PADDED) {
                    if (!n) throw std::string("bad padding");
                    pad = *d++;
                    n--;
                }
                if (flags & PRIORITY_FLAG) {
                    if (n < 5) throw std::string("bad HEADERS");
                    d += 5;
                    n -= 5;
                }
                if (pad > n) throw std::string("bad padding");
                block.assign((const char *)d, n - pad);
                block_stream = sid;
                block_end_stream = flags & END_STREAM;
                if (flags & END_HEADERS) on_headers(sid, block, block_end_stream);
                else in_block = true;
                break;
            }
            case CONTINUATION:
                if (!in_block || sid != block_stream) throw std::string("unexpected CONTINUATION");
                block.append((const char *)p, len);
                if (flags & END_HEADERS) {
                    in_block = false;
                    on_headers(sid, block, block_end_stream);
                }
                break;
            case RST_STREAM: {
                if (len != 4) throw std::string("bad RST_STREAM");
                uint32_t code = u32(p);
                std::lock_guard<std::mutex> lk(m_);
                for (auto it = streams_.begin(); it != streams_.end(); ++it) {
                    if ((*it)->id != sid) continue;
                    Stream &s = **it;
                    {
                        std::lock_guard<std::mutex> sl(s.m);
                        s.failed = true;
                        s.error = "the server reset the request (HTTP/2 error " + std::to_string(code) + ")";
                        s.retry_ok = code == 7;  // REFUSED_STREAM: not processed, try again
                        s.cv.notify_all();
                    }
                    streams_.erase(it);
                    cv_.notify_all();
                    break;
                }
                break;
            }
            case SETTINGS: {
                if (sid != 0 || len % 6) throw std::string("bad SETTINGS");
                if (flags & ACK) break;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    for (uint32_t i = 0; i < len; i += 6) {
                        uint16_t id = (uint16_t)(p[i] << 8 | p[i + 1]);
                        uint32_t v = u32(p + i + 2);
                        if (id == 3) peer_max_streams_ = std::max<uint32_t>(v, 1);
                        else if (id == 4) {
                            if (v > 0x7fffffff) throw std::string("bad window size");
                            for (auto &s : streams_) s->send_window += (int64_t)v - peer_initial_window_;
                            peer_initial_window_ = v;
                        } else if (id == 5) {
                            if (v < 16384 || v > 16777215) throw std::string("bad frame size");
                            peer_max_frame_ = v;
                        }
                    }
                    cv_.notify_all();
                }
                std::lock_guard<std::mutex> w(wm_);
                write_frame(SETTINGS, ACK, 0, nullptr, 0);
                break;
            }
            case PING:
                if (len != 8) throw std::string("bad PING");
                if (!(flags & ACK)) {
                    std::lock_guard<std::mutex> w(wm_);
                    write_frame(PING, ACK, 0, p, 8);
                }
                break;
            case GOAWAY: {
                if (len < 8) throw std::string("bad GOAWAY");
                uint32_t last = u32(p) & 0x7fffffff;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    going_away_ = true;
                }
                fail_all("the server is closing the connection", last);  // later streams: never processed
                break;
            }
            case WINDOW_UPDATE: {
                if (len != 4) throw std::string("bad WINDOW_UPDATE");
                uint32_t inc = u32(p) & 0x7fffffff;
                std::lock_guard<std::mutex> lk(m_);
                if (sid == 0) send_window_ += inc;
                else
                    for (auto &s : streams_)
                        if (s->id == sid) s->send_window += inc;
                cv_.notify_all();
                break;
            }
            case PUSH_PROMISE:
                throw std::string("server push, which we turned off");
            default:  // PRIORITY and unknown frame types: nothing to do
                break;
            }
        }
    } catch (const std::string &e) {
        why = "HTTP/2 protocol error: " + e;
        std::string payload = be32(0) + be32(1);  // GOAWAY, PROTOCOL_ERROR
        std::lock_guard<std::mutex> w(wm_);
        write_frame(GOAWAY, 0, 0, payload);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        dead_ = true;
    }
    fail_all(why, 0);
    std::lock_guard<std::mutex> reg(g_reg_m);
    g_conns.erase(std::remove(g_conns.begin(), g_conns.end(), shared_from_this()), g_conns.end());
    shutdown(c_->s, SD_BOTH);
}

std::shared_ptr<Connection> find(const std::string &key) {
    std::lock_guard<std::mutex> lk(g_reg_m);
    for (auto &c : g_conns)
        if (c->key() == key && c->usable()) return c;
    return nullptr;
}

std::shared_ptr<Connection> adopt(std::unique_ptr<net::Conn> conn, const std::string &key) {
    auto c = std::make_shared<Connection>(std::move(conn), key);
    if (!c->start()) return nullptr;
    std::lock_guard<std::mutex> lk(g_reg_m);
    g_conns.push_back(c);
    return c;
}

}  // namespace h2
