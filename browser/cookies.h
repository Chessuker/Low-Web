// cookies.h — the cookie jar (RFC 6265): remembers Set-Cookie answers and adds a Cookie
// header to later requests. Persistent cookies are saved in a file; session cookies last
// until the browser closes.
//
// Privacy rules, like current browsers: cookies go only to the site the page belongs to
// ("third-party" cookies are neither sent nor stored), SameSite is honoured (Lax when a
// cookie doesn't say), and Secure cookies travel only over HTTPS.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cookies {

// `file` empty = keep nothing on disk (cookies still work until the process ends).
void init(const std::wstring &file);

// Who is asking: the page (top-level document) a request is made for.
struct Context {
    std::string site_for;    // the page's URL; empty = the user (typed address, bookmark): same-site
    bool navigation = false; // loading a page in a tab (not a subresource)
    bool unsafe_method = false;  // POST
};

// The value for a Cookie header for this request ("" = none).
std::string header_for(const std::string &url, const Context &ctx);

// Stores the cookies from a response's Set-Cookie header lines.
void store(const std::string &url, const std::vector<std::string> &set_cookie, const Context &ctx);

// Same site = same registrable domain (example.co.th for www.example.co.th).
bool same_site(const std::string &host_a, const std::string &host_b);
std::string registrable_domain(const std::string &host);

void clear();
size_t count();

}  // namespace cookies
