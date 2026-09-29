// svg.cpp — a small SVG renderer: XML parsing, styles (attributes, style="", simple <style>
// rules), shapes and paths (with arcs), transforms, viewBox, <use>/<symbol>, solid and
// gradient paints, strokes (joins, caps, dashes) and an anti-aliased scanline rasterizer.
// Not supported: text, filters, masks, clipping paths, patterns, markers.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "image.h"

namespace image {

namespace {

struct SvgError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

const char *find_str(const char *h, size_t n, const char *needle) {
    size_t k = std::strlen(needle);
    for (size_t i = 0; i + k <= n; i++)
        if (!std::memcmp(h + i, needle, k)) return h + i;
    return nullptr;
}

// =====================================================================================
// XML
// =====================================================================================

struct XNode {
    std::string name;  // lower case, namespace prefix removed
    std::vector<std::pair<std::string, std::string>> attrs;  // names lower case
    std::vector<std::unique_ptr<XNode>> children;
    std::string text;
    XNode *parent = nullptr;

    const std::string *attr(const char *n) const {
        for (auto &a : attrs)
            if (a.first == n) return &a.second;
        return nullptr;
    }
};

std::string decode_entities(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { o += s[i]; continue; }
        std::string e = s.substr(i + 1, semi - i - 1);
        uint32_t cp = 0;
        if (e == "amp") cp = '&';
        else if (e == "lt") cp = '<';
        else if (e == "gt") cp = '>';
        else if (e == "quot") cp = '"';
        else if (e == "apos") cp = '\'';
        else if (!e.empty() && e[0] == '#') cp = (uint32_t)strtoul(e.c_str() + (e[1] == 'x' ? 2 : 1), nullptr, e[1] == 'x' ? 16 : 10);
        else { o += s[i]; continue; }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
        i = semi;
    }
    return o;
}

std::unique_ptr<XNode> parse_xml(const char *d, size_t n) {
    auto root = std::make_unique<XNode>();
    root->name = "#root";
    XNode *cur = root.get();
    size_t i = 0;
    int depth = 0;
    while (i < n) {
        if (d[i] != '<') {
            size_t j = i;
            while (j < n && d[j] != '<') j++;
            if (cur != root.get()) cur->text += decode_entities(std::string(d + i, j - i));
            i = j;
            continue;
        }
        if (i + 4 <= n && !std::memcmp(d + i, "<!--", 4)) {
            const char *e = find_str(d + i + 4, n - i - 4, "-->");
            i = e ? (size_t)(e - d) + 3 : n;
            continue;
        }
        if (i + 9 <= n && !std::memcmp(d + i, "<![CDATA[", 9)) {
            const char *e = find_str(d + i + 9, n - i - 9, "]]>");
            size_t end = e ? (size_t)(e - d) : n;
            cur->text.append(d + i + 9, end - i - 9);
            i = e ? end + 3 : n;
            continue;
        }
        if (i + 1 < n && (d[i + 1] == '?' || d[i + 1] == '!')) {  // declarations, DOCTYPE (with [...])
            int bracket = 0;
            size_t j = i + 2;
            while (j < n && !(d[j] == '>' && bracket == 0)) {
                if (d[j] == '[') bracket++;
                if (d[j] == ']') bracket--;
                j++;
            }
            i = j + 1;
            continue;
        }
        if (i + 1 < n && d[i + 1] == '/') {  // end tag
            size_t j = i + 2;
            while (j < n && d[j] != '>') j++;
            if (cur->parent) { cur = cur->parent; depth--; }
            i = j + 1;
            continue;
        }
        // start tag
        size_t j = i + 1;
        std::string name;
        while (j < n && !isspace((unsigned char)d[j]) && d[j] != '>' && d[j] != '/') name += d[j++];
        size_t colon = name.find(':');
        if (colon != std::string::npos) name = name.substr(colon + 1);
        auto node = std::make_unique<XNode>();
        node->name = lower(name);
        node->parent = cur;
        bool self_close = false;
        for (;;) {
            while (j < n && isspace((unsigned char)d[j])) j++;
            if (j >= n) break;
            if (d[j] == '>') { j++; break; }
            if (d[j] == '/') { self_close = true; j++; continue; }
            std::string an;
            while (j < n && !isspace((unsigned char)d[j]) && d[j] != '=' && d[j] != '>' && d[j] != '/') an += d[j++];
            while (j < n && isspace((unsigned char)d[j])) j++;
            std::string av;
            if (j < n && d[j] == '=') {
                j++;
                while (j < n && isspace((unsigned char)d[j])) j++;
                if (j < n && (d[j] == '"' || d[j] == '\'')) {
                    char q = d[j++];
                    size_t s = j;
                    while (j < n && d[j] != q) j++;
                    av.assign(d + s, j - s);
                    j++;
                } else {
                    size_t s = j;
                    while (j < n && !isspace((unsigned char)d[j]) && d[j] != '>') j++;
                    av.assign(d + s, j - s);
                }
            }
            if (!an.empty()) node->attrs.push_back({lower(an), decode_entities(av)});
        }
        XNode *raw = node.get();
        cur->children.push_back(std::move(node));
        if (!self_close && depth < 256) { cur = raw; depth++; }
        i = j;
    }
    return root;
}

// =====================================================================================
// geometry
// =====================================================================================

struct Mat {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;  // x' = a x + c y + e; y' = b x + d y + f
    Mat operator*(const Mat &m) const {  // this after m... (this * m): apply m first, then this
        Mat r;
        r.a = a * m.a + c * m.b;
        r.b = b * m.a + d * m.b;
        r.c = a * m.c + c * m.d;
        r.d = b * m.c + d * m.d;
        r.e = a * m.e + c * m.f + e;
        r.f = b * m.e + d * m.f + f;
        return r;
    }
    void apply(double x, double y, double &ox, double &oy) const {
        ox = a * x + c * y + e;
        oy = b * x + d * y + f;
    }
    double scale() const { return std::sqrt(std::fabs(a * d - b * c)); }
    bool invert(Mat &r) const {
        double det = a * d - b * c;
        if (std::fabs(det) < 1e-12) return false;
        r.a = d / det; r.b = -b / det; r.c = -c / det; r.d = a / det;
        r.e = (c * f - d * e) / det;
        r.f = (b * e - a * f) / det;
        return true;
    }
    static Mat translate(double x, double y) { Mat m; m.e = x; m.f = y; return m; }
    static Mat scaled(double x, double y) { Mat m; m.a = x; m.d = y; return m; }
};

struct Pt { double x, y; };
struct Poly {
    std::vector<Pt> pts;
    bool closed = false;
};

// Number scanner for SVG's compact syntax ("1.5.5", "-1-2", "1e-3").
struct Nums {
    const char *p, *e;
    explicit Nums(const std::string &s) : p(s.c_str()), e(s.c_str() + s.size()) {}
    void skip() { while (p < e && (isspace((unsigned char)*p) || *p == ',')) p++; }
    bool number(double &v) {
        skip();
        const char *s = p;
        if (p < e && (*p == '+' || *p == '-')) p++;
        bool digits = false, dot = false;
        while (p < e && (isdigit((unsigned char)*p) || (*p == '.' && !dot))) {
            if (*p == '.') dot = true;
            else digits = true;
            p++;
        }
        if (!digits) { p = s; return false; }
        if (p < e && (*p == 'e' || *p == 'E') && p + 1 < e && (isdigit((unsigned char)p[1]) || ((p[1] == '-' || p[1] == '+') && p + 2 < e && isdigit((unsigned char)p[2])))) {
            p += 2;
            while (p < e && isdigit((unsigned char)*p)) p++;
        }
        v = strtod(std::string(s, p).c_str(), nullptr);
        return true;
    }
    bool flag(int &v) {
        skip();
        if (p < e && (*p == '0' || *p == '1')) { v = *p++ - '0'; return true; }
        return false;
    }
};

double parse_length(const std::string &s, double percent_of, double def) {
    Nums n(s);
    double v;
    if (!n.number(v)) return def;
    std::string unit = lower(trim(std::string(n.p, n.e)));
    if (unit == "%") return v * percent_of / 100;
    if (unit == "pt") return v * 4 / 3;
    if (unit == "pc") return v * 16;
    if (unit == "mm") return v * 96 / 25.4;
    if (unit == "cm") return v * 96 / 2.54;
    if (unit == "in") return v * 96;
    if (unit == "em" || unit == "rem") return v * 16;
    if (unit == "ex") return v * 8;
    return v;
}

Mat parse_transform(const std::string &s) {
    Mat m;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (isspace((unsigned char)s[i]) || s[i] == ',')) i++;
        size_t name_start = i;
        while (i < s.size() && isalpha((unsigned char)s[i])) i++;
        std::string name = lower(s.substr(name_start, i - name_start));
        size_t open = s.find('(', i), close = s.find(')', i);
        if (name.empty() || open == std::string::npos || close == std::string::npos) break;
        Nums n(s.substr(open + 1, close - open - 1));
        std::vector<double> v;
        double x;
        while (n.number(x)) v.push_back(x);
        Mat t;
        const double rad = 3.14159265358979323846 / 180;
        if (name == "matrix" && v.size() == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        else if (name == "translate" && !v.empty()) t = Mat::translate(v[0], v.size() > 1 ? v[1] : 0);
        else if (name == "scale" && !v.empty()) t = Mat::scaled(v[0], v.size() > 1 ? v[1] : v[0]);
        else if (name == "rotate" && !v.empty()) {
            Mat r;
            double c = std::cos(v[0] * rad), sn = std::sin(v[0] * rad);
            r.a = c; r.b = sn; r.c = -sn; r.d = c;
            if (v.size() >= 3) t = Mat::translate(v[1], v[2]) * r * Mat::translate(-v[1], -v[2]);
            else t = r;
        } else if (name == "skewx" && !v.empty()) t.c = std::tan(v[0] * rad);
        else if (name == "skewy" && !v.empty()) t.b = std::tan(v[0] * rad);
        m = m * t;
        i = close + 1;
    }
    return m;
}

// Path data -> polylines (curves flattened with `tol` user units per segment step).
struct PathBuilder {
    std::vector<Poly> polys;
    double tol;
    Pt cur{0, 0}, start{0, 0};
    explicit PathBuilder(double t) : tol(t) {}

    void move(Pt p) {
        if (!polys.empty() && polys.back().pts.size() < 2 && !polys.back().closed) polys.pop_back();
        polys.push_back({});
        polys.back().pts.push_back(p);
        cur = start = p;
    }
    void line(Pt p) {
        if (polys.empty()) move(cur);
        polys.back().pts.push_back(p);
        cur = p;
    }
    void cubic(Pt c1, Pt c2, Pt p) {
        double len = std::hypot(c1.x - cur.x, c1.y - cur.y) + std::hypot(c2.x - c1.x, c2.y - c1.y) + std::hypot(p.x - c2.x, p.y - c2.y);
        int n = std::clamp((int)std::ceil(len / tol), 2, 256);
        Pt p0 = cur;
        for (int i = 1; i <= n; i++) {
            double t = (double)i / n, u = 1 - t;
            line({u * u * u * p0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * p.x,
                  u * u * u * p0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * p.y});
        }
    }
    void quad(Pt c, Pt p) {
        Pt p0 = cur;
        cubic({p0.x + 2.0 / 3 * (c.x - p0.x), p0.y + 2.0 / 3 * (c.y - p0.y)}, {p.x + 2.0 / 3 * (c.x - p.x), p.y + 2.0 / 3 * (c.y - p.y)}, p);
    }
    // SVG arc (endpoint parameterization) -> cubic segments
    void arc(double rx, double ry, double angle, int large, int sweep, Pt p) {
        Pt p0 = cur;
        if (rx == 0 || ry == 0) { line(p); return; }
        if (p0.x == p.x && p0.y == p.y) return;
        rx = std::fabs(rx);
        ry = std::fabs(ry);
        double phi = angle * 3.14159265358979323846 / 180, cs = std::cos(phi), sn = std::sin(phi);
        double dx = (p0.x - p.x) / 2, dy = (p0.y - p.y) / 2;
        double x1 = cs * dx + sn * dy, y1 = -sn * dx + cs * dy;
        double lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
        double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double co = den == 0 ? 0 : std::sqrt(std::max(0.0, num / den));
        if (large == sweep) co = -co;
        double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
        double cx = cs * cxp - sn * cyp + (p0.x + p.x) / 2, cy = sn * cxp + cs * cyp + (p0.y + p.y) / 2;
        auto ang = [](double ux, double uy, double vx, double vy) {
            double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            return a;
        };
        double t1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        double dt = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && dt > 0) dt -= 2 * 3.14159265358979323846;
        if (sweep && dt < 0) dt += 2 * 3.14159265358979323846;
        int segs = (int)std::ceil(std::fabs(dt) / (3.14159265358979323846 / 2));
        double step = dt / segs;
        for (int i = 0; i < segs; i++) {
            double a1 = t1 + i * step, a2 = a1 + step;
            double k = 4.0 / 3 * std::tan(step / 4);
            auto pt = [&](double a) { return Pt{cx + rx * std::cos(a) * cs - ry * std::sin(a) * sn, cy + rx * std::cos(a) * sn + ry * std::sin(a) * cs}; };
            auto dv = [&](double a) { return Pt{-rx * std::sin(a) * cs - ry * std::cos(a) * sn, -rx * std::sin(a) * sn + ry * std::cos(a) * cs}; };
            Pt e1 = pt(a1), e2 = pt(a2), d1 = dv(a1), d2 = dv(a2);
            cubic({e1.x + k * d1.x, e1.y + k * d1.y}, {e2.x - k * d2.x, e2.y - k * d2.y}, i == segs - 1 ? p : e2);
        }
    }
    void close() {
        if (!polys.empty()) {
            polys.back().closed = true;
            cur = start;
        }
    }
};

void parse_path(const std::string &d, PathBuilder &pb) {
    Nums n(d);
    char cmd = 0;
    Pt last_ctrl{0, 0};
    char last_cmd = 0;
    while (true) {
        n.skip();
        if (n.p >= n.e) break;
        if (isalpha((unsigned char)*n.p)) cmd = *n.p++;
        else if (!cmd) break;
        bool rel = islower((unsigned char)cmd);
        char C = (char)toupper((unsigned char)cmd);
        Pt c = pb.cur;
        double v[7];
        auto nums = [&](int k) {
            for (int i = 0; i < k; i++)
                if (!n.number(v[i])) return false;
            return true;
        };
        if (C == 'Z') {
            pb.close();
            last_cmd = 'Z';
            cmd = 0;
            continue;
        }
        if (C == 'M') {
            if (!nums(2)) break;
            pb.move({rel ? c.x + v[0] : v[0], rel ? c.y + v[1] : v[1]});
            cmd = rel ? 'l' : 'L';  // further pairs are line-tos
        } else if (C == 'L') {
            if (!nums(2)) break;
            pb.line({rel ? c.x + v[0] : v[0], rel ? c.y + v[1] : v[1]});
        } else if (C == 'H') {
            if (!nums(1)) break;
            pb.line({rel ? c.x + v[0] : v[0], c.y});
        } else if (C == 'V') {
            if (!nums(1)) break;
            pb.line({c.x, rel ? c.y + v[0] : v[0]});
        } else if (C == 'C') {
            if (!nums(6)) break;
            Pt c1{rel ? c.x + v[0] : v[0], rel ? c.y + v[1] : v[1]}, c2{rel ? c.x + v[2] : v[2], rel ? c.y + v[3] : v[3]};
            Pt p{rel ? c.x + v[4] : v[4], rel ? c.y + v[5] : v[5]};
            pb.cubic(c1, c2, p);
            last_ctrl = c2;
        } else if (C == 'S') {
            if (!nums(4)) break;
            Pt c1 = (last_cmd == 'C' || last_cmd == 'S') ? Pt{2 * c.x - last_ctrl.x, 2 * c.y - last_ctrl.y} : c;
            Pt c2{rel ? c.x + v[0] : v[0], rel ? c.y + v[1] : v[1]}, p{rel ? c.x + v[2] : v[2], rel ? c.y + v[3] : v[3]};
            pb.cubic(c1, c2, p);
            last_ctrl = c2;
        } else if (C == 'Q') {
            if (!nums(4)) break;
            Pt q{rel ? c.x + v[0] : v[0], rel ? c.y + v[1] : v[1]}, p{rel ? c.x + v[2] : v[2], rel ? c.y + v[3] : v[3]};
            pb.quad(q, p);
            last_ctrl = q;
        } else if (C == 'T') {
            if (!nums(2)) break;
            Pt q = (last_cmd == 'Q' || last_cmd == 'T') ? Pt{2 * c.x - last_ctrl.x, 2 * c.y - last_ctrl.y} : c;
            Pt p{rel ? c.x + v[0] : v[0], rel ? c.y + v[1] : v[1]};
            pb.quad(q, p);
            last_ctrl = q;
        } else if (C == 'A') {
            int large, sweep;
            if (!n.number(v[0]) || !n.number(v[1]) || !n.number(v[2]) || !n.flag(large) || !n.flag(sweep) || !n.number(v[3]) || !n.number(v[4])) break;
            pb.arc(v[0], v[1], v[2], large, sweep, {rel ? c.x + v[3] : v[3], rel ? c.y + v[4] : v[4]});
        } else {
            break;
        }
        last_cmd = C;
    }
}

// =====================================================================================
// paint and colour
// =====================================================================================

struct Color {
    double r = 0, g = 0, b = 0, a = 1;
};

bool named_color(const std::string &n, Color &c) {
    static const std::map<std::string, uint32_t> names = {
        {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff},
        {"yellow", 0xffff00}, {"orange", 0xffa500}, {"purple", 0x800080}, {"gray", 0x808080}, {"grey", 0x808080},
        {"silver", 0xc0c0c0}, {"maroon", 0x800000}, {"navy", 0x000080}, {"teal", 0x008080}, {"olive", 0x808000},
        {"lime", 0x00ff00}, {"aqua", 0x00ffff}, {"cyan", 0x00ffff}, {"fuchsia", 0xff00ff}, {"magenta", 0xff00ff},
        {"pink", 0xffc0cb}, {"brown", 0xa52a2a}, {"gold", 0xffd700}, {"darkgray", 0xa9a9a9}, {"darkgrey", 0xa9a9a9},
        {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3}, {"darkblue", 0x00008b}, {"darkred", 0x8b0000},
        {"darkgreen", 0x006400}, {"skyblue", 0x87ceeb}, {"steelblue", 0x4682b4}, {"tomato", 0xff6347},
        {"coral", 0xff7f50}, {"crimson", 0xdc143c}, {"indigo", 0x4b0082}, {"violet", 0xee82ee}, {"tan", 0xd2b48c},
        {"beige", 0xf5f5dc}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"salmon", 0xfa8072}, {"orchid", 0xda70d6},
        {"plum", 0xdda0dd}, {"turquoise", 0x40e0d0}, {"slategray", 0x708090}, {"dimgray", 0x696969},
        {"whitesmoke", 0xf5f5f5}, {"gainsboro", 0xdcdcdc}, {"royalblue", 0x4169e1}, {"dodgerblue", 0x1e90ff},
        {"forestgreen", 0x228b22}, {"seagreen", 0x2e8b57}, {"firebrick", 0xb22222}, {"chocolate", 0xd2691e},
        {"goldenrod", 0xdaa520}, {"lightblue", 0xadd8e6}, {"lightgreen", 0x90ee90}, {"darkorange", 0xff8c00},
    };
    auto it = names.find(n);
    if (it == names.end()) return false;
    c = {(it->second >> 16 & 255) / 255.0, (it->second >> 8 & 255) / 255.0, (it->second & 255) / 255.0, 1};
    return true;
}

bool parse_color(std::string s, Color &c, const Color &current) {
    s = lower(trim(s));
    if (s.empty()) return false;
    if (s == "currentcolor") { c = current; return true; }
    if (s == "transparent") { c = {0, 0, 0, 0}; return true; }
    if (s[0] == '#') {
        std::string h = s.substr(1);
        auto hv = [&](int i) { return (int)strtol(h.substr(i, 1).c_str(), nullptr, 16); };
        auto hv2 = [&](int i) { return (int)strtol(h.substr(i, 2).c_str(), nullptr, 16); };
        for (char ch : h) if (!isxdigit((unsigned char)ch)) return false;
        if (h.size() == 3 || h.size() == 4) {
            c = {hv(0) * 17 / 255.0, hv(1) * 17 / 255.0, hv(2) * 17 / 255.0, h.size() == 4 ? hv(3) * 17 / 255.0 : 1};
            return true;
        }
        if (h.size() == 6 || h.size() == 8) {
            c = {hv2(0) / 255.0, hv2(2) / 255.0, hv2(4) / 255.0, h.size() == 8 ? hv2(6) / 255.0 : 1};
            return true;
        }
        return false;
    }
    if (s.compare(0, 3, "rgb") == 0 || s.compare(0, 3, "hsl") == 0) {
        size_t open = s.find('('), close = s.find(')');
        if (open == std::string::npos) return false;
        std::string inner = s.substr(open + 1, close == std::string::npos ? std::string::npos : close - open - 1);
        for (char &ch : inner) if (ch == '/' || ch == ',') ch = ' ';
        std::vector<double> v;
        std::vector<bool> pct;
        size_t i = 0;
        while (i < inner.size()) {
            while (i < inner.size() && inner[i] == ' ') i++;
            if (i >= inner.size()) break;
            char *end;
            double x = strtod(inner.c_str() + i, &end);
            if (end == inner.c_str() + i) break;
            i = (size_t)(end - inner.c_str());
            bool p = i < inner.size() && inner[i] == '%';
            if (p) i++;
            while (i < inner.size() && isalpha((unsigned char)inner[i])) i++;  // "deg"
            v.push_back(x);
            pct.push_back(p);
        }
        if (v.size() < 3) return false;
        double a = v.size() > 3 ? (pct[3] ? v[3] / 100 : v[3]) : 1;
        if (s[0] == 'r') {
            auto ch = [&](int k) { return std::clamp(pct[k] ? v[k] / 100 : v[k] / 255, 0.0, 1.0); };
            c = {ch(0), ch(1), ch(2), a};
        } else {
            double hh = std::fmod(v[0] / 360 + 1, 1.0), ss = v[1] / 100, ll = v[2] / 100;
            auto f = [&](double n) {
                double k = std::fmod(n + hh * 12, 12.0), am = ss * std::min(ll, 1 - ll);
                return ll - am * std::max(-1.0, std::min({k - 3, 9 - k, 1.0}));
            };
            c = {f(0), f(8), f(4), a};
        }
        return true;
    }
    return named_color(s, c);
}

struct Paint {
    enum { NONE, SOLID, GRADIENT } kind = NONE;
    Color color;
    const XNode *grad = nullptr;
};

// =====================================================================================
// rasterizer
// =====================================================================================

struct Edge {
    double x0, y0, x1, y1;
    int dir;
};

struct Canvas {
    int w, h;
    std::vector<float> px;  // premultiplied RGBA, 0..1
};

void add_poly_edges(std::vector<Edge> &edges, const std::vector<Pt> &pts, const Mat &m) {
    size_t n = pts.size();
    if (n < 2) return;
    for (size_t i = 0; i < n; i++) {
        const Pt &a = pts[i], &b = pts[(i + 1) % n];
        double x0, y0, x1, y1;
        m.apply(a.x, a.y, x0, y0);
        m.apply(b.x, b.y, x1, y1);
        if (y0 == y1) continue;
        if (y0 < y1) edges.push_back({x0, y0, x1, y1, 1});
        else edges.push_back({x1, y1, x0, y0, -1});
    }
}

void fill_edges(Canvas &cv, std::vector<Edge> &edges, bool evenodd, const std::function<Color(double, double)> &paint, double opacity) {
    if (edges.empty()) return;
    std::sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.y0 < b.y0; });
    double ymin = edges.front().y0, ymax = 0, xmin = 1e30, xmax = -1e30;
    for (auto &e : edges) {
        ymax = std::max(ymax, e.y1);
        xmin = std::min({xmin, e.x0, e.x1});
        xmax = std::max({xmax, e.x0, e.x1});
    }
    int y_start = std::max(0, (int)std::floor(ymin)), y_end = std::min(cv.h, (int)std::ceil(ymax));
    int x_lo = std::max(0, (int)std::floor(xmin)), x_hi = std::min(cv.w, (int)std::ceil(xmax) + 1);
    if (x_lo >= x_hi) return;
    const int SS = 5;
    std::vector<float> cov(cv.w + 2), acc(cv.w + 2);
    std::vector<std::pair<double, int>> xs;
    std::vector<const Edge *> active;
    size_t next = 0;
    for (int y = y_start; y < y_end; y++) {
        std::fill(cov.begin() + x_lo, cov.begin() + x_hi + 1, 0.f);
        std::fill(acc.begin() + x_lo, acc.begin() + x_hi + 1, 0.f);
        while (next < edges.size() && edges[next].y0 < y + 1) active.push_back(&edges[next++]);
        active.erase(std::remove_if(active.begin(), active.end(), [&](const Edge *e) { return e->y1 <= y; }), active.end());
        bool any = false;
        for (int s = 0; s < SS; s++) {
            double sy = y + (s + 0.5) / SS;
            xs.clear();
            for (const Edge *e : active)
                if (e->y0 <= sy && sy < e->y1) xs.push_back({e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0), e->dir});
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < xs.size(); i++) {
                wind += xs[i].second;
                bool inside = evenodd ? (wind & 1) : wind != 0;
                if (!inside) continue;
                double xa = std::clamp(xs[i].first, (double)x_lo, (double)x_hi), xb = std::clamp(xs[i + 1].first, (double)x_lo, (double)x_hi);
                if (xb <= xa) continue;
                any = true;
                const float wt = 1.0f / SS;
                int ia = (int)xa, ib = (int)xb;
                if (ia == ib) cov[ia] += (float)(xb - xa) * wt;
                else {
                    cov[ia] += (float)(ia + 1 - xa) * wt;
                    acc[ia + 1] += wt;
                    acc[ib] -= wt;
                    cov[ib] += (float)(xb - ib) * wt;
                }
            }
        }
        if (!any) continue;
        float run = 0;
        float *row = &cv.px[(size_t)y * cv.w * 4];
        for (int x = x_lo; x < x_hi; x++) {
            run += acc[x];
            float c = std::min(1.0f, cov[x] + run);
            if (c <= 0.001f) continue;
            Color col = paint(x + 0.5, y + 0.5);
            float a = (float)(col.a * opacity) * c;
            if (a <= 0) continue;
            float *o = row + x * 4;
            float na = 1 - a;
            o[0] = (float)col.r * a + o[0] * na;
            o[1] = (float)col.g * a + o[1] * na;
            o[2] = (float)col.b * a + o[2] * na;
            o[3] = a + o[3] * na;
        }
    }
}

// =====================================================================================
// the renderer
// =====================================================================================

struct Rule {
    std::string tag, cls, id;  // empty = any
    int spec, order;
    std::vector<std::pair<std::string, std::string>> decls;
};

struct State {
    Paint fill, stroke;
    double fill_op = 1, stroke_op = 1, opacity = 1, stroke_w = 1, miter = 4;
    int cap = 0, join = 0;  // cap: butt, round, square; join: miter, round, bevel
    bool evenodd = false, visible = true;
    Color current{0, 0, 0, 1};
    std::vector<double> dash;
    double dash_offset = 0;
    Mat m;
};

void parse_decls(const std::string &s, std::vector<std::pair<std::string, std::string>> &out) {
    size_t i = 0;
    while (i < s.size()) {
        size_t semi = s.find(';', i);
        std::string d = s.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        size_t colon = d.find(':');
        if (colon != std::string::npos) {
            std::string v = trim(d.substr(colon + 1));
            size_t imp = v.find("!important");
            if (imp != std::string::npos) v = trim(v.substr(0, imp));
            out.push_back({lower(trim(d.substr(0, colon))), v});
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
}

struct Renderer {
    Canvas cv;
    std::map<std::string, const XNode *> ids;
    std::vector<Rule> rules;
    int depth = 0;

    void index(const XNode *n) {
        if (auto id = n->attr("id")) ids[*id] = n;
        if (n->name == "style") parse_css(n->text);
        for (auto &c : n->children) index(c.get());
    }

    void parse_css(const std::string &css) {
        size_t i = 0;
        std::string t = css;
        for (size_t c; (c = t.find("/*")) != std::string::npos;) {  // strip comments
            size_t e = t.find("*/", c + 2);
            t.erase(c, e == std::string::npos ? std::string::npos : e + 2 - c);
        }
        while (i < t.size()) {
            size_t open = t.find('{', i);
            if (open == std::string::npos) break;
            size_t close = t.find('}', open);
            if (close == std::string::npos) close = t.size();
            std::string sel = trim(t.substr(i, open - i));
            std::vector<std::pair<std::string, std::string>> decls;
            parse_decls(t.substr(open + 1, close - open - 1), decls);
            if (!sel.empty() && sel[0] != '@') {
                size_t k = 0;
                while (k <= sel.size()) {
                    size_t comma = sel.find(',', k);
                    std::string one = trim(sel.substr(k, comma == std::string::npos ? std::string::npos : comma - k));
                    Rule r;
                    bool ok = !one.empty();
                    size_t p = 0;
                    auto ident = [&]() { size_t s = p; while (p < one.size() && (isalnum((unsigned char)one[p]) || one[p] == '-' || one[p] == '_')) p++; return one.substr(s, p - s); };
                    if (p < one.size() && (isalpha((unsigned char)one[p]) || one[p] == '*')) {
                        if (one[p] == '*') p++;
                        else r.tag = lower(ident());
                    }
                    while (ok && p < one.size()) {
                        if (one[p] == '.') { p++; r.cls = ident(); }
                        else if (one[p] == '#') { p++; r.id = ident(); }
                        else ok = false;  // combinators, pseudo-classes: not supported
                    }
                    if (ok) {
                        r.spec = (r.id.empty() ? 0 : 100) + (r.cls.empty() ? 0 : 10) + (r.tag.empty() ? 0 : 1);
                        r.order = (int)rules.size();
                        r.decls = decls;
                        rules.push_back(r);
                    }
                    if (comma == std::string::npos) break;
                    k = comma + 1;
                }
            }
            i = close + 1;
        }
    }

    // presentation attributes < <style> rules < style=""
    std::map<std::string, std::string> props(const XNode *n) {
        std::map<std::string, std::string> p;
        static const char *pres[] = {"fill", "fill-opacity", "fill-rule", "stroke", "stroke-width", "stroke-opacity",
                                     "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
                                     "stroke-dashoffset", "opacity", "display", "visibility", "color", "stop-color",
                                     "stop-opacity", "transform"};
        for (const char *k : pres)
            if (auto v = n->attr(k)) p[k] = *v;
        if (!rules.empty()) {
            std::vector<const Rule *> hit;
            const std::string *cls = n->attr("class"), *id = n->attr("id");
            for (const Rule &r : rules) {
                if (!r.tag.empty() && r.tag != n->name) continue;
                if (!r.id.empty() && (!id || *id != r.id)) continue;
                if (!r.cls.empty()) {
                    if (!cls) continue;
                    bool found = false;
                    size_t i = 0;
                    while (i < cls->size() && !found) {
                        while (i < cls->size() && isspace((unsigned char)(*cls)[i])) i++;
                        size_t s = i;
                        while (i < cls->size() && !isspace((unsigned char)(*cls)[i])) i++;
                        found = cls->compare(s, i - s, r.cls) == 0 && i - s == r.cls.size();
                    }
                    if (!found) continue;
                }
                hit.push_back(&r);
            }
            std::stable_sort(hit.begin(), hit.end(), [](const Rule *a, const Rule *b) { return a->spec < b->spec; });
            for (const Rule *r : hit)
                for (auto &d : r->decls) p[d.first] = d.second;
        }
        if (auto st = n->attr("style")) {
            std::vector<std::pair<std::string, std::string>> d;
            parse_decls(*st, d);
            for (auto &kv : d) p[kv.first] = kv.second;
        }
        return p;
    }

    Paint parse_paint(const std::string &v, const State &st) {
        Paint p;
        std::string s = trim(v);
        if (s.empty() || lower(s) == "none") return p;
        if (lower(s).compare(0, 4, "url(") == 0) {
            size_t close = s.find(')');
            std::string ref = trim(s.substr(4, close - 4));
            if (!ref.empty() && (ref[0] == '"' || ref[0] == '\'')) ref = ref.substr(1, ref.size() - 2);
            if (!ref.empty() && ref[0] == '#') {
                auto it = ids.find(ref.substr(1));
                if (it != ids.end() && (it->second->name == "lineargradient" || it->second->name == "radialgradient")) {
                    p.kind = Paint::GRADIENT;
                    p.grad = it->second;
                    return p;
                }
            }
            std::string fallback = close == std::string::npos ? "" : trim(s.substr(close + 1));
            if (!fallback.empty() && parse_color(fallback, p.color, st.current)) p.kind = Paint::SOLID;
            return p;
        }
        if (parse_color(s, p.color, st.current)) p.kind = Paint::SOLID;
        return p;
    }

    void apply_props(const XNode *n, State &st) {
        auto p = props(n);
        auto get = [&](const char *k) -> const std::string * {
            auto it = p.find(k);
            return it == p.end() || lower(trim(it->second)) == "inherit" ? nullptr : &it->second;
        };
        if (auto v = get("color")) parse_color(*v, st.current, st.current);
        if (auto v = get("fill")) st.fill = parse_paint(*v, st);
        if (auto v = get("stroke")) st.stroke = parse_paint(*v, st);
        if (auto v = get("fill-opacity")) st.fill_op = std::clamp(strtod(v->c_str(), nullptr) * (v->find('%') != std::string::npos ? 0.01 : 1), 0.0, 1.0);
        if (auto v = get("stroke-opacity")) st.stroke_op = std::clamp(strtod(v->c_str(), nullptr) * (v->find('%') != std::string::npos ? 0.01 : 1), 0.0, 1.0);
        if (auto v = get("opacity")) st.opacity *= std::clamp(strtod(v->c_str(), nullptr) * (v->find('%') != std::string::npos ? 0.01 : 1), 0.0, 1.0);
        if (auto v = get("fill-rule")) st.evenodd = lower(trim(*v)) == "evenodd";
        if (auto v = get("stroke-width")) st.stroke_w = std::max(0.0, parse_length(*v, 100, 1));
        if (auto v = get("stroke-miterlimit")) st.miter = std::max(1.0, strtod(v->c_str(), nullptr));
        if (auto v = get("stroke-linecap")) { std::string c = lower(trim(*v)); st.cap = c == "round" ? 1 : c == "square" ? 2 : 0; }
        if (auto v = get("stroke-linejoin")) { std::string j = lower(trim(*v)); st.join = j == "round" ? 1 : j == "bevel" ? 2 : 0; }
        if (auto v = get("stroke-dasharray")) {
            st.dash.clear();
            if (lower(trim(*v)) != "none") {
                Nums nn(*v);
                double x;
                while (nn.number(x)) {
                    st.dash.push_back(std::max(0.0, x));
                    while (nn.p < nn.e && (isalpha((unsigned char)*nn.p) || *nn.p == '%')) nn.p++;
                }
                if (st.dash.size() % 2) { auto d = st.dash; st.dash.insert(st.dash.end(), d.begin(), d.end()); }
                double total = 0;
                for (double dd : st.dash) total += dd;
                if (total <= 0) st.dash.clear();
            }
        }
        if (auto v = get("stroke-dashoffset")) st.dash_offset = parse_length(*v, 100, 0);
        if (auto v = get("visibility")) st.visible = lower(trim(*v)) == "visible";
        auto t = p.find("transform");
        if (t != p.end()) st.m = st.m * parse_transform(t->second);
    }

    // Gradient paint at a device pixel.
    Color gradient_at(const XNode *g, double x, double y, const Mat &m, double bx, double by, double bw, double bh) {
        // collect attributes and stops, following href chains
        std::map<std::string, std::string> a;
        const XNode *stops_from = nullptr;
        for (const XNode *cur = g; cur && a.size() < 64;) {
            for (auto &kv : cur->attrs)
                if (!a.count(kv.first)) a[kv.first] = kv.second;
            if (!stops_from)
                for (auto &c : cur->children)
                    if (c->name == "stop") { stops_from = cur; break; }
            const std::string *href = cur->attr("href");
            if (!href) href = cur->attr("xlink:href");
            if (!href || href->empty() || (*href)[0] != '#') break;
            auto it = ids.find(href->substr(1));
            if (it == ids.end() || it->second == cur) break;
            cur = it->second;
        }
        struct Stop { double off; Color c; };
        std::vector<Stop> stops;
        if (stops_from) {
            for (auto &c : stops_from->children) {
                if (c->name != "stop") continue;
                auto p = props(c.get());
                Stop s;
                std::string off = c->attr("offset") ? *c->attr("offset") : "0";
                s.off = strtod(off.c_str(), nullptr) * (off.find('%') != std::string::npos ? 0.01 : 1);
                s.off = std::clamp(s.off, 0.0, 1.0);
                if (!stops.empty()) s.off = std::max(s.off, stops.back().off);
                s.c = {0, 0, 0, 1};
                if (p.count("stop-color")) parse_color(p["stop-color"], s.c, Color{0, 0, 0, 1});
                if (p.count("stop-opacity")) s.c.a *= std::clamp(strtod(p["stop-opacity"].c_str(), nullptr), 0.0, 1.0);
                stops.push_back(s);
            }
        }
        if (stops.empty()) return {0, 0, 0, 0};
        if (stops.size() == 1) return stops[0].c;
        bool user_space = a.count("gradientunits") && a["gradientunits"] == "userSpaceOnUse";
        Mat gt = a.count("gradienttransform") ? parse_transform(a["gradienttransform"]) : Mat();
        Mat inv;
        if (!m.invert(inv)) return stops[0].c;
        double ux, uy;
        inv.apply(x, y, ux, uy);  // user space
        if (!user_space) {
            if (bw <= 0 || bh <= 0) return stops[0].c;
            ux = (ux - bx) / bw;
            uy = (uy - by) / bh;
        }
        Mat ginv;
        if (gt.invert(ginv)) ginv.apply(ux, uy, ux, uy);
        auto len = [&](const char *k, double def, double pct_of) {
            if (!a.count(k)) return def;
            const std::string &v = a[k];
            double n = strtod(v.c_str(), nullptr);
            if (v.find('%') != std::string::npos) return n / 100 * (user_space ? pct_of : 1);
            return n;
        };
        double t;
        if (g->name == "lineargradient") {
            double x1 = len("x1", 0, cv.w), y1 = len("y1", 0, cv.h), x2 = len("x2", user_space ? cv.w : 1, cv.w), y2 = len("y2", 0, cv.h);
            double dx = x2 - x1, dy = y2 - y1, dd = dx * dx + dy * dy;
            t = dd > 0 ? ((ux - x1) * dx + (uy - y1) * dy) / dd : 0;
        } else {
            double cx = len("cx", user_space ? cv.w / 2.0 : 0.5, cv.w), cy = len("cy", user_space ? cv.h / 2.0 : 0.5, cv.h);
            double r = len("r", user_space ? cv.w / 2.0 : 0.5, cv.w);
            t = r > 0 ? std::hypot(ux - cx, uy - cy) / r : 0;
        }
        std::string spread = a.count("spreadmethod") ? a["spreadmethod"] : "pad";
        if (spread == "repeat") t -= std::floor(t);
        else if (spread == "reflect") { t = std::fmod(std::fabs(t), 2.0); if (t > 1) t = 2 - t; }
        t = std::clamp(t, 0.0, 1.0);
        if (t <= stops.front().off) return stops.front().c;
        if (t >= stops.back().off) return stops.back().c;
        for (size_t i = 1; i < stops.size(); i++)
            if (t <= stops[i].off) {
                const Stop &s0 = stops[i - 1], &s1 = stops[i];
                double f = s1.off > s0.off ? (t - s0.off) / (s1.off - s0.off) : 0;
                return {s0.c.r + (s1.c.r - s0.c.r) * f, s0.c.g + (s1.c.g - s0.c.g) * f, s0.c.b + (s1.c.b - s0.c.b) * f,
                        s0.c.a + (s1.c.a - s0.c.a) * f};
            }
        return stops.back().c;
    }

    void paint_polys(std::vector<Poly> &polys, const Paint &paint, double op, bool evenodd, const Mat &m, const std::vector<Poly> &bbox_src) {
        if (paint.kind == Paint::NONE || op <= 0) return;
        std::vector<Edge> edges;
        for (auto &p : polys) add_poly_edges(edges, p.pts, m);
        double bx = 1e30, by = 1e30, bx2 = -1e30, by2 = -1e30;
        for (auto &p : bbox_src)
            for (auto &q : p.pts) { bx = std::min(bx, q.x); by = std::min(by, q.y); bx2 = std::max(bx2, q.x); by2 = std::max(by2, q.y); }
        if (paint.kind == Paint::SOLID) {
            Color c = paint.color;
            fill_edges(cv, edges, evenodd, [c](double, double) { return c; }, op);
        } else {
            const XNode *g = paint.grad;
            fill_edges(cv, edges, evenodd, [&](double x, double y) { return gradient_at(g, x, y, m, bx, by, bx2 - bx, by2 - by); }, op);
        }
    }

    static void circle_poly(std::vector<Poly> &out, Pt c, double r, int segs) {
        Poly p;
        p.closed = true;
        for (int i = 0; i < segs; i++) {
            double a = 2 * 3.14159265358979323846 * i / segs;
            p.pts.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
        }
        out.push_back(p);
    }

    static void orient(Poly &p) {  // same winding everywhere, so nonzero fill = union
        double area = 0;
        for (size_t i = 0; i < p.pts.size(); i++) {
            const Pt &a = p.pts[i], &b = p.pts[(i + 1) % p.pts.size()];
            area += a.x * b.y - b.x * a.y;
        }
        if (area < 0) std::reverse(p.pts.begin(), p.pts.end());
    }

    std::vector<Poly> dashed(const std::vector<Poly> &in, const std::vector<double> &dash, double offset) {
        std::vector<Poly> out;
        double total = 0;
        for (double d : dash) total += d;
        for (const Poly &p : in) {
            std::vector<Pt> pts = p.pts;
            if (p.closed && !pts.empty()) pts.push_back(pts[0]);
            size_t di = 0;
            double pos = std::fmod(offset, total);
            if (pos < 0) pos += total;
            while (pos >= dash[di]) { pos -= dash[di]; di = (di + 1) % dash.size(); }
            double left = dash[di] - pos;
            bool on = !(di & 1);
            Poly cur;
            if (on && !pts.empty()) cur.pts.push_back(pts[0]);
            for (size_t i = 0; i + 1 < pts.size(); i++) {
                Pt a = pts[i], b = pts[i + 1];
                double seg = std::hypot(b.x - a.x, b.y - a.y), done = 0;
                while (seg - done > left) {
                    done += left;
                    Pt q{a.x + (b.x - a.x) * done / seg, a.y + (b.y - a.y) * done / seg};
                    if (on) { cur.pts.push_back(q); out.push_back(cur); cur.pts.clear(); }
                    else cur.pts.push_back(q);
                    on = !on;
                    di = (di + 1) % dash.size();
                    left = dash[di];
                }
                left -= seg - done;
                if (on) cur.pts.push_back(b);
            }
            if (on && cur.pts.size() >= 2) out.push_back(cur);
        }
        return out;
    }

    // Stroke outline polygons (user space).
    std::vector<Poly> stroke_polys(const std::vector<Poly> &in, const State &st, double scale) {
        std::vector<Poly> out;
        double hw = st.stroke_w / 2;
        if (hw <= 0) return out;
        int segs = std::clamp((int)(hw * scale * 2), 8, 64);
        std::vector<Poly> lines = st.dash.empty() ? in : dashed(in, st.dash, st.dash_offset);
        for (const Poly &pl : lines) {
            std::vector<Pt> pts;
            for (const Pt &p : pl.pts)
                if (pts.empty() || std::hypot(p.x - pts.back().x, p.y - pts.back().y) > 1e-9) pts.push_back(p);
            bool closed = pl.closed && st.dash.empty();
            if (closed && pts.size() > 2 && std::hypot(pts[0].x - pts.back().x, pts[0].y - pts.back().y) < 1e-9) pts.pop_back();
            if (pts.size() == 1) {
                if (st.cap == 1) circle_poly(out, pts[0], hw, segs);
                else if (st.cap == 2) out.push_back({{{pts[0].x - hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y - hw}, {pts[0].x + hw, pts[0].y + hw}, {pts[0].x - hw, pts[0].y + hw}}, true});
                continue;
            }
            size_t n = pts.size(), nseg = closed ? n : n - 1;
            for (size_t i = 0; i < nseg; i++) {
                Pt a = pts[i], b = pts[(i + 1) % n];
                double dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy);
                double ux = dx / l, uy = dy / l, nx = -uy * hw, ny = ux * hw;
                if (!closed && st.cap == 2) {
                    if (i == 0) { a.x -= ux * hw; a.y -= uy * hw; }
                    if (i == nseg - 1) { b.x += ux * hw; b.y += uy * hw; }
                }
                Poly q;
                q.closed = true;
                q.pts = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny}, {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
                out.push_back(q);
            }
            // joins
            for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
                Pt p0 = pts[(i + n - 1) % n], p1 = pts[i], p2 = pts[(i + 1) % n];
                double d0x = p1.x - p0.x, d0y = p1.y - p0.y, d1x = p2.x - p1.x, d1y = p2.y - p1.y;
                double l0 = std::hypot(d0x, d0y), l1 = std::hypot(d1x, d1y);
                if (l0 < 1e-12 || l1 < 1e-12) continue;
                d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
                if (st.join == 1) { circle_poly(out, p1, hw, segs); continue; }
                double cross = d0x * d1y - d0y * d1x;
                if (std::fabs(cross) < 1e-9) continue;
                double s = cross > 0 ? -1 : 1;  // the outer side
                Pt a{p1.x + s * -d0y * hw, p1.y + s * d0x * hw}, b{p1.x + s * -d1y * hw, p1.y + s * d1x * hw};
                Poly q;
                q.closed = true;
                if (st.join == 0) {
                    double cos_half = std::sqrt(std::max(0.0, (1 + (d0x * d1x + d0y * d1y)) / 2));
                    if (cos_half > 1e-6 && 1 / cos_half <= st.miter) {
                        double mx = (a.x + b.x) / 2 - p1.x, my = (a.y + b.y) / 2 - p1.y, ml = std::hypot(mx, my);
                        if (ml > 1e-12) {
                            double k = hw / cos_half / ml;
                            q.pts = {p1, a, {p1.x + mx * k, p1.y + my * k}, b};
                            out.push_back(q);
                            continue;
                        }
                    }
                }
                q.pts = {p1, a, b};
                out.push_back(q);
            }
            if (!closed && st.cap == 1) {
                circle_poly(out, pts[0], hw, segs);
                circle_poly(out, pts[n - 1], hw, segs);
            }
        }
        for (Poly &p : out) orient(p);
        return out;
    }

    void draw_shape(std::vector<Poly> polys, const State &st) {
        if (!st.visible || polys.empty()) return;
        std::vector<Poly> fill_polys;
        for (auto &p : polys)
            if (p.pts.size() >= 3) fill_polys.push_back(p);
        paint_polys(fill_polys, st.fill, st.fill_op * st.opacity, st.evenodd, st.m, polys);
        if (st.stroke.kind != Paint::NONE) {
            std::vector<Poly> sp = stroke_polys(polys, st, st.m.scale());
            paint_polys(sp, st.stroke, st.stroke_op * st.opacity, false, st.m, polys);
        }
    }

    double attr_len(const XNode *n, const char *k, double pct_of, double def) {
        const std::string *v = n->attr(k);
        return v ? parse_length(*v, pct_of, def) : def;
    }

    // viewBox + preserveAspectRatio -> transform into a w x h viewport
    Mat viewbox_transform(const XNode *n, double w, double h) {
        const std::string *vb = n->attr("viewbox");
        if (!vb) return Mat();
        Nums nn(*vb);
        double v[4];
        for (double &x : v)
            if (!nn.number(x)) return Mat();
        if (v[2] <= 0 || v[3] <= 0) return Mat();
        std::string par = n->attr("preserveaspectratio") ? lower(*n->attr("preserveaspectratio")) : "xmidymid meet";
        double sx = w / v[2], sy = h / v[3];
        if (par.find("none") == std::string::npos) {
            double s = par.find("slice") != std::string::npos ? std::max(sx, sy) : std::min(sx, sy);
            double tx = 0, ty = 0;
            if (par.find("xmid") != std::string::npos) tx = (w - v[2] * s) / 2;
            else if (par.find("xmax") != std::string::npos) tx = w - v[2] * s;
            if (par.find("ymid") != std::string::npos) ty = (h - v[3] * s) / 2;
            else if (par.find("ymax") != std::string::npos) ty = h - v[3] * s;
            return Mat::translate(tx, ty) * Mat::scaled(s, s) * Mat::translate(-v[0], -v[1]);
        }
        return Mat::scaled(sx, sy) * Mat::translate(-v[0], -v[1]);
    }

    void render(const XNode *n, State st, bool from_use = false) {
        if (++depth > 64) { depth--; return; }
        const std::string &name = n->name;
        static const char *skip[] = {"defs", "clippath", "mask", "pattern", "lineargradient", "radialgradient", "marker",
                                     "style", "title", "desc", "metadata", "filter", "text", "script", "foreignobject"};
        for (const char *s : skip)
            if (name == s) { depth--; return; }
        if (name == "symbol" && !from_use) { depth--; return; }
        auto p = props(n);
        if (p.count("display") && lower(trim(p["display"])) == "none") { depth--; return; }
        apply_props(n, st);
        double tol = 0.3 / std::max(st.m.scale(), 1e-6);
        if (name == "g" || name == "a" || name == "#root" || name == "symbol") {
            for (auto &c : n->children) render(c.get(), st);
        } else if (name == "switch") {
            for (auto &c : n->children)
                if (c->name != "desc" && c->name != "title") { render(c.get(), st); break; }
        } else if (name == "svg") {
            if (n->parent && n->parent->name != "#root") {  // nested viewport
                double x = attr_len(n, "x", cv.w, 0), y = attr_len(n, "y", cv.h, 0);
                double w = attr_len(n, "width", cv.w, cv.w), h = attr_len(n, "height", cv.h, cv.h);
                st.m = st.m * Mat::translate(x, y) * viewbox_transform(n, w, h);
            }
            for (auto &c : n->children) render(c.get(), st);
        } else if (name == "use") {
            const std::string *href = n->attr("href");
            if (!href) href = n->attr("xlink:href");
            if (href && !href->empty() && (*href)[0] == '#') {
                auto it = ids.find(href->substr(1));
                if (it != ids.end() && it->second != n) {
                    st.m = st.m * Mat::translate(attr_len(n, "x", cv.w, 0), attr_len(n, "y", cv.h, 0));
                    if (it->second->name == "symbol")
                        st.m = st.m * viewbox_transform(it->second, attr_len(n, "width", cv.w, cv.w), attr_len(n, "height", cv.h, cv.h));
                    render(it->second, st, true);
                }
            }
        } else {
            PathBuilder pb(tol);
            if (name == "path") {
                if (auto d = n->attr("d")) parse_path(*d, pb);
            } else if (name == "rect") {
                double x = attr_len(n, "x", cv.w, 0), y = attr_len(n, "y", cv.h, 0);
                double w = attr_len(n, "width", cv.w, 0), h = attr_len(n, "height", cv.h, 0);
                double rx = attr_len(n, "rx", cv.w, -1), ry = attr_len(n, "ry", cv.h, -1);
                if (rx < 0) rx = ry;
                if (ry < 0) ry = rx;
                rx = std::clamp(rx, 0.0, w / 2);
                ry = std::clamp(ry, 0.0, h / 2);
                if (w > 0 && h > 0) {
                    if (rx > 0 && ry > 0) {
                        pb.move({x + rx, y});
                        pb.line({x + w - rx, y});
                        pb.arc(rx, ry, 0, 0, 1, {x + w, y + ry});
                        pb.line({x + w, y + h - ry});
                        pb.arc(rx, ry, 0, 0, 1, {x + w - rx, y + h});
                        pb.line({x + rx, y + h});
                        pb.arc(rx, ry, 0, 0, 1, {x, y + h - ry});
                        pb.line({x, y + ry});
                        pb.arc(rx, ry, 0, 0, 1, {x + rx, y});
                    } else {
                        pb.move({x, y});
                        pb.line({x + w, y});
                        pb.line({x + w, y + h});
                        pb.line({x, y + h});
                    }
                    pb.close();
                }
            } else if (name == "circle" || name == "ellipse") {
                double cx = attr_len(n, "cx", cv.w, 0), cy = attr_len(n, "cy", cv.h, 0);
                double rx = name == "circle" ? attr_len(n, "r", std::hypot(cv.w, cv.h) / std::sqrt(2.0), 0) : attr_len(n, "rx", cv.w, 0);
                double ry = name == "circle" ? rx : attr_len(n, "ry", cv.h, 0);
                if (rx > 0 && ry > 0) {
                    pb.move({cx + rx, cy});
                    pb.arc(rx, ry, 0, 0, 1, {cx - rx, cy});
                    pb.arc(rx, ry, 0, 0, 1, {cx + rx, cy});
                    pb.close();
                }
            } else if (name == "line") {
                pb.move({attr_len(n, "x1", cv.w, 0), attr_len(n, "y1", cv.h, 0)});
                pb.line({attr_len(n, "x2", cv.w, 0), attr_len(n, "y2", cv.h, 0)});
            } else if (name == "polyline" || name == "polygon") {
                if (auto pts = n->attr("points")) {
                    Nums nn(*pts);
                    double x, y;
                    bool first = true;
                    while (nn.number(x) && nn.number(y)) {
                        if (first) pb.move({x, y});
                        else pb.line({x, y});
                        first = false;
                    }
                    if (name == "polygon") pb.close();
                }
            }
            draw_shape(pb.polys, st);
        }
        depth--;
    }
};

}  // namespace

bool is_svg(const uint8_t *d, size_t n) {
    size_t lim = std::min<size_t>(n, 4096);
    std::string head((const char *)d, lim);
    size_t first = head.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (first == std::string::npos || head[first] != '<') return false;
    return lower(head).find("<svg") != std::string::npos;
}

void decode_svg(const uint8_t *d, size_t n, Image &img) {
    std::unique_ptr<XNode> doc = parse_xml((const char *)d, n);
    const XNode *svg = nullptr;
    for (auto &c : doc->children)
        if (c->name == "svg") { svg = c.get(); break; }
    if (!svg) throw SvgError("no <svg> element");
    // intrinsic size: width/height, else the viewBox, else 300 x 150
    double vbw = 0, vbh = 0;
    if (auto vb = svg->attr("viewbox")) {
        Nums nn(*vb);
        double v[4];
        if (nn.number(v[0]) && nn.number(v[1]) && nn.number(v[2]) && nn.number(v[3])) { vbw = v[2]; vbh = v[3]; }
    }
    auto dim = [&](const char *k, double fallback) {
        const std::string *v = svg->attr(k);
        if (!v || v->find('%') != std::string::npos) return fallback;
        double x = parse_length(*v, 0, -1);
        return x > 0 ? x : fallback;
    };
    double w = dim("width", 0), h = dim("height", 0);
    if (w <= 0 && h <= 0) { w = vbw > 0 ? vbw : 300; h = vbh > 0 ? vbh : 150; }
    else if (w <= 0) w = vbh > 0 ? h * vbw / vbh : h;
    else if (h <= 0) h = vbw > 0 ? w * vbh / vbw : w;
    double scale = std::min(1.0, 2048.0 / std::max(w, h));
    int iw = std::max(1, (int)std::lround(w * scale)), ih = std::max(1, (int)std::lround(h * scale));

    Renderer r;
    r.cv.w = iw;
    r.cv.h = ih;
    r.cv.px.assign((size_t)iw * ih * 4, 0.f);
    r.index(doc.get());
    State st;
    st.fill.kind = Paint::SOLID;  // default fill: black
    st.m = Mat::scaled(scale, scale) * r.viewbox_transform(svg, w, h);
    for (auto &c : svg->children) {
        State s2 = st;
        r.apply_props(svg, s2);  // root presentation attributes
        s2.m = st.m;
        r.render(c.get(), s2);
    }
    img.w = iw;
    img.h = ih;
    img.rgba.resize((size_t)iw * ih * 4);
    for (size_t i = 0; i < (size_t)iw * ih; i++) {
        const float *p = &r.cv.px[i * 4];
        float a = p[3];
        uint8_t *o = &img.rgba[i * 4];
        if (a <= 0) { o[0] = o[1] = o[2] = o[3] = 0; continue; }
        o[0] = (uint8_t)std::lround(std::clamp(p[0] / a, 0.f, 1.f) * 255);
        o[1] = (uint8_t)std::lround(std::clamp(p[1] / a, 0.f, 1.f) * 255);
        o[2] = (uint8_t)std::lround(std::clamp(p[2] / a, 0.f, 1.f) * 255);
        o[3] = (uint8_t)std::lround(std::clamp(a, 0.f, 1.f) * 255);
    }
}

}  // namespace image
