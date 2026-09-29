// cookietest [FILE] — checks the cookie jar's rules (browser/cookies.cpp) without a network.
// Run by tests/check_cookies.py.
#include <cstdio>
#include <string>
#include <vector>

#include "../browser/cookies.h"

static int failed = 0;

static void check(const char *what, const std::string &got, const std::string &want) {
    bool ok = got == want;
    printf("%s %s", ok ? "ok  " : "FAIL", what);
    if (!ok) printf("   got \"%s\", want \"%s\"", got.c_str(), want.c_str());
    printf("\n");
    failed += !ok;
}

static void set(const std::string &url, std::vector<std::string> lines, cookies::Context ctx = {}) { cookies::store(url, lines, ctx); }
static std::string get(const std::string &url, cookies::Context ctx = {}) { return cookies::header_for(url, ctx); }

int main(int argc, char **argv) {
    std::string file = argc > 1 ? argv[1] : "";
    std::wstring wfile(file.begin(), file.end());
    cookies::init(wfile);
    cookies::clear();

    // basics
    set("https://www.example.com/shop/cart", {"a=1", "b=2; Path=/", "c=3; Path=/shop/cart/items"});
    check("default path is the request's directory", get("https://www.example.com/shop/x"), "a=1; b=2");
    check("... not sent outside it", get("https://www.example.com/other"), "b=2");
    check("longer paths first", get("https://www.example.com/shop/cart/items/9"), "c=3; a=1; b=2");
    check("host-only: not sent to another host", get("https://example.com/"), "");

    // Domain
    set("https://www.example.com/", {"d=4; Domain=.example.com; Path=/"});
    check("Domain=example.com reaches sub-domains", get("https://img.example.com/"), "d=4");
    set("https://www.example.com/", {"x=1; Domain=com; Path=/", "y=1; Domain=other.com; Path=/"});
    check("Domain of a public suffix or another site: refused", get("https://other.com/") + get("https://www.com/"), "");
    set("https://shop.example.co.th/", {"t=1; Domain=co.th; Path=/", "u=2; Domain=example.co.th; Path=/"});
    check("... also two-level suffixes (co.th)", get("https://news.example.co.th/"), "u=2");
    check("registrable domain", cookies::registrable_domain("www.example.co.th") + " " + cookies::registrable_domain("a.b.github.io") +
          " " + cookies::registrable_domain("en.wikipedia.org"), "example.co.th b.github.io wikipedia.org");

    // Secure
    set("http://plain.test/", {"s=1; Secure; Path=/"});
    check("Secure cookie from http: refused", get("https://plain.test/"), "");
    set("https://sec.test/", {"s=2; Secure; Path=/", "p=3; Path=/"});
    check("Secure cookie not sent over http", get("http://sec.test/"), "p=3");

    // lifetimes
    set("https://life.test/", {"m=1; Path=/; Max-Age=3600", "n=2; Path=/; Expires=Wed, 01 Jan 2031 00:00:00 GMT",
                               "o=3; Path=/; Max-Age=3600; Expires=Wed, 01 Jan 1997 00:00:00 GMT"});
    check("Max-Age, Expires, Max-Age wins over Expires", get("https://life.test/"), "m=1; n=2; o=3");
    set("https://life.test/", {"m=; Path=/; Max-Age=0", "n=x; Path=/; Expires=Wed, 01 Jan 1997 00:00:00 GMT"});
    check("Max-Age=0 or a past date deletes", get("https://life.test/"), "o=3");
    set("https://life.test/", {"o=33; Path=/; Max-Age=60"});
    check("same name, domain and path: replaced", get("https://life.test/"), "o=33");

    // SameSite and third parties
    set("https://bank.test/", {"lax=1; Path=/", "strict=2; Path=/; SameSite=Strict", "none=3; Path=/; SameSite=None; Secure",
                               "badnone=4; Path=/; SameSite=None"});
    cookies::Context same{"https://www.bank.test/page", false, false};
    check("same site (sub-domain page): all", get("https://bank.test/", same), "lax=1; strict=2; none=3");
    cookies::Context nav{"https://evil.test/", true, false};
    check("link from another site: Lax and None, not Strict", get("https://bank.test/", nav), "lax=1; none=3");
    cookies::Context post{"https://evil.test/", true, true};
    check("form POST from another site: only None", get("https://bank.test/", post), "none=3");
    cookies::Context third{"https://evil.test/", false, false};
    check("image/script for another site's page: nothing (third-party)", get("https://bank.test/", third), "");
    set("https://tracker.test/", {"id=42; Path=/; SameSite=None; Secure"}, third);
    check("... and nothing stored", get("https://tracker.test/"), "");
    cookies::Context user{};
    check("typed address: all", get("https://bank.test/", user), "lax=1; strict=2; none=3");

    // prefixes
    set("https://pre.test/a/", {"__Host-ok=1; Secure; Path=/", "__Host-dom=2; Secure; Path=/; Domain=pre.test",
                                "__Host-path=3; Secure; Path=/a", "__Secure-ok=4; Secure; Path=/", "__Secure-no=5; Path=/"});
    check("__Host- and __Secure- prefixes", get("https://pre.test/a/b"), "__Host-ok=1; __Secure-ok=4");

    // saving: persistent cookies come back, session cookies don't
    if (!file.empty()) {
        cookies::clear();
        set("https://keep.test/", {"session=1; Path=/", "saved=2; Path=/; Max-Age=86400"});
        cookies::init(wfile);  // as if the browser started again
        check("persistent cookies are saved, session cookies are not", get("https://keep.test/"), "saved=2");
        cookies::clear();
    }
    printf(failed ? "%d FAILED\n" : "ALL OK\n", failed);
    return failed ? 1 : 0;
}
