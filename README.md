# Low-web

[![CI](https://github.com/Chessuker/Low-Web/actions/workflows/ci.yml/badge.svg)](https://github.com/Chessuker/Low-Web/actions/workflows/ci.yml)

A web without HTML, CSS or JavaScript, and a browser written from scratch for it.

A Low-web page is a single WebAssembly program (`index.wasm`) that draws every pixel itself.
The browser (plain C++, no framework) is the "machine" that program runs on: it downloads it
over HTTP/HTTPS, runs it in its own interpreter, and puts the pixels it produces on the screen.

Ordinary HTML sites open too: the browser hands the HTML document to the **viewer**, which is
just another Low-web page (written in C) that reads HTML and draws it as a reader view. The
browser itself still knows nothing about HTML.

```
 ┌──────────── lowweb.exe (C++, Win32) ─────────────┐        ┌──── lowd (C++) ────┐
 │ address bar ─► net.cpp  HTTP/1.1, HTTP/2 + TLS    ├──TCP──►│ static files       │
 │                 │        gzip (inflate.cpp)       │        │ /  → /index.wasm   │
 │                 ▼                                 │        └────────────────────┘
 │   index.wasm? ──► run that page                                                 │
 │   HTML?  ───────► viewer.wasm (built into the exe) + the HTML document          │
 │                                                                                 │
 │ wasm.cpp  WebAssembly interpreter  ◄── the page (C → wasm32)                    │
 │   │  imports "lw.*"  (sdk/lowweb.h)                                             │
 │   ├─ lw_present ──► framebuffer ─► StretchDIBits (screen)                       │
 │   ├─ lw_text ─────► GDI + Uniscribe (every script, Thai word breaking included) │
 │   ├─ lw_image_decode ► PNG / JPEG / GIF / WebP / SVG / BMP (all written here)   │
 │   ├─ lw_video_* ──► Windows Media Foundation (video.cpp: decoded on the GPU)    │
 │   └─ lw_fetch / lw_navigate(_post) / lw_open_file / lw_save_file               │
 └──────────────────────────────────────────────────┘
```

## Layout of the repository

| Folder | What |
|---|---|
| `browser/` | The browser: `main.cpp` (window, tabs, address bar, host of the page ABI), `wasm.cpp` (interpreter), `net.cpp` (URLs, HTTP/1.1 + keep-alive), `http2.cpp` (HTTP/2 + HPACK), `conn.h` (TCP + TLS through SChannel), `cache.cpp` (HTTP cache), `cookies.cpp` (cookies), `inflate.cpp` (DEFLATE/gzip), `image.cpp` (PNG/JPEG/GIF/BMP), `webp.cpp` (WebP lossy + lossless), `svg.cpp` (SVG renderer), `video.cpp` (video/audio through Media Foundation) |
| `server/lowd.cpp` | A web server for hosting Low-web sites (Windows + Linux) |
| `sdk/lowweb.h` | **The page ABI**: the contract between a page and the browser |
| `sdk/hello.c` | The smallest example page (~1 KB), to start from |
| `sites/home/`, `sites/paint/` | Sources of the example pages |
| `sites/viewer/` | **The HTML viewer**: `viewer.c` (HTML → layout → drawing) and `css.c` (as much CSS as it needs) |
| `sites/www/` | **The web root** that gets hosted (`index.wasm`, `paint/index.wasm`, `motd.txt`, `about.txt`) |
| `tests/` | Tests of the interpreter, decoders, network and cache; sample files (`tests/samples/`: `gen_*` are made by `tests/gen_samples.py` from pictures drawn there but written by real encoders, so they look like files on the web; `real_w3c.svg` (CC-BY, credit inside the file), the Thai flag, RFC 7541); development scripts (`dev.sh`, `shot.sh`, `slow_server.py`) |
| `build/` | What the build makes (`viewer.wasm`, `lowweb_res.o`, `lowd.exe`, `ops.wasm`) and temporary test output; the whole folder can be deleted |
| `legacy/paint-html/` | The old version of Paint, which still relied on HTML + JS |

## Build

You need **g++ (MinGW-w64)** and **clang with the wasm32 target** (LLVM).

```bat
build.cmd
```

This makes `bin\lowweb.exe` (the browser, with the viewer built in), `bin\lowd.exe` (the server),
`sites\www\**\index.wasm` and the test tools. If `lowd.exe` is running, the build doesn't
overwrite it (the new one is in `build\lowd.exe`).

While working on the viewer: put `viewer.wasm` next to `lowweb.exe` and the browser uses that
file instead of the built-in one, so there is no need to rebuild the exe (`bash tests/dev.sh`
does it; `bash tests/shot.sh NAME URL` opens a page and saves a PNG screenshot in `build/shots/`).

## Use

```bat
bin\lowweb.exe                         :: the example site, straight from disk (sites\www)
bin\lowd.exe sites\www                 :: a server on port 8080
bin\lowweb.exe http://localhost:8080/  :: the same over the real network
bin\lowweb.exe https://en.wikipedia.org/wiki/Bangkok   :: an ordinary HTML site (reader view)
```

Keys:

| Key | What it does |
|---|---|
| `Ctrl+L` | Go to the address bar (type an address or words to search for) |
| `Ctrl+T` / `Ctrl+W` | Open / close a tab |
| `Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+1`…`9` | Switch tabs |
| `Ctrl+Shift+T` | Reopen the tab closed last |
| `Alt+Enter` in the address bar | Open in a new tab |
| Middle click / `Ctrl`+click on a link | Open the link in a background tab |
| `F5` · `Ctrl+F5` · `Alt+←/→` · `Alt+Home` | Reload (asks the server whether it changed) · reload without the cache · back/forward · home |
| `F11` · `Esc` | Fullscreen (no tab strip, no toolbar) · leave fullscreen |
| Right-click · menu key / `Shift+F10` | A menu for what is under the mouse: a link (open in a new tab, copy its address), an image (open, save, copy its address), a video (play/pause, mute, copy its address), copy the selection, select all, reader view/full page; then back, forward, reload |
| `Ctrl+F` · `Enter`/`F3` · `Shift+Enter`/`Shift+F3` · `Esc` | Find in page (a bar at the top right shows "current/total", case-insensitive) · next · previous · close |

Drop a `.wasm` file on the window to run it · drop a picture on Paint to open it.

**Selecting, copying, pasting text**: drag to select, double-click = a word, triple-click = the
paragraph, `Shift`+click = extend the selection, `Ctrl+A` = the whole page, `Ctrl+C` copies (with
the page's line breaks); dragging past the top or bottom edge scrolls. In text fields: `Ctrl+V`
pastes, `Ctrl+A` then `Ctrl+C`/`Ctrl+X` copies/cuts (password fields can't be copied). The pages
the browser shows itself (text files, error pages) can be selected and copied as well.

**Text fields**: a caret and a selection: `←/→` (`Ctrl` = by word), `Home/End`, `Shift` + these
= select, `Backspace`/`Delete` (`Ctrl` = a whole word), click to place the caret, drag to select,
double-click = a word, triple-click = all of it, `Tab`/`Shift+Tab` = next/previous field. Long
text scrolls with the caret; `<textarea>` wraps its lines, `Enter` starts a new line, `↑/↓` move by
line; Thai vowels and tone marks stay with the letter before them (`Backspace` removes them one
at a time).
Low-web pages use the clipboard through `lw_clipboard_set`/`lw_clipboard_get`: they can write it
only while handling the user's input, and read it only while the user presses `Ctrl+V`/`Shift+Insert`,
so a page can't read or overwrite the clipboard behind the user's back.

**The tab strip is the title bar**: the window has no Windows title bar; the tab strip is at the
very top, as in other browsers. It has its own minimize/maximize/close buttons on the right; drag
an empty part of it to move the window, double-click there to maximize, right-click there for the
window menu, drag the top edge to resize. Aero Snap and `Win+←/→/↑` work as usual.

**Searching from the address bar**: if what you type isn't an address (it has spaces, no dot, or
starts with `?`), it is searched for. The small button next to the address bar picks the search
engine: **DuckDuckGo** (the default), **Google**, **Bing**. The choice is kept in
`%APPDATA%\Low-web\settings.ini`. (Google's result pages now need JavaScript, so you get its
"turn on JavaScript" page instead of results.)

**Network, cache and cookies**
- **HTTP/2** (RFC 9113) for HTTPS servers that offer it (agreed through ALPN during the TLS
  handshake): every request to the same server shares one connection (a Wikipedia page: 3
  connections instead of 10); HPACK is written here, Huffman coding included (the table is
  generated from RFC 7541 by `tests/gen_hpack_huffman.py`); servers that answer with HTTP/1.1 are
  remembered, so there is no need to try again every time.
- **Keep-alive** (HTTP/1.1): a finished connection is kept for the next request to the same server
  (at most 16, for 60 seconds), which saves the TCP + TLS handshakes; if the server has closed a
  kept connection meanwhile, the request is sent again on a new one.
- **TLS session resumption**: a new connection to a server seen before uses the short handshake
  (Wikipedia: 78 → 31 ms).
- **Cookies** (RFC 6265): `Domain`, `Path`, `Secure`, `HttpOnly`, `Max-Age`/`Expires`, `SameSite`
  (`Lax` when not given), the `__Host-`/`__Secure-` prefixes, no cookies for public suffixes
  (`.com`, `.co.th` …); **cookies of other sites (third-party) are neither sent nor stored**;
  cookies with an expiry date are kept in `%LOCALAPPDATA%\Low-web\cookies.txt`, session cookies go
  away when the browser closes.
- **User-Agent** in Chrome's form with `Low-web/0.1` at the end (sites behind bot walls and CDNs
  don't block it; DuckDuckGo's html search used to answer with a bot check) and **Referer** as
  browsers send it by default (`strict-origin-when-cross-origin`: the whole address to the same
  origin, only the origin to others, none from HTTPS to HTTP, from files or for typed addresses),
  so videos and pictures from sites with hotlink protection no longer get 403. A page's own
  `Referrer-Policy` / `<meta name=referrer>` isn't read yet.
- **HTTP cache**: follows `Cache-Control` (`max-age`, `no-cache`, `no-store`), `Expires`,
  `ETag`/`If-None-Match`, `Last-Modified`/`If-Modified-Since` (when the server gives no lifetime,
  10% of the age since `Last-Modified`, at most a day). Kept in memory (16 MB, at most 2 MB per
  file) and on disk in `%LOCALAPPDATA%\Low-web\Cache` (at most 256 MB; the least recently used
  files go first).
- A page that moves on by itself (not from a click or a key, e.g. a redirect page's
  `<meta refresh>`) takes its own place in the history: Back from where it led goes to the page
  before the redirect (as in Chrome).
- `F5` asks the server whether the page changed, `Ctrl+F5` / `Ctrl+Shift+R` load everything again,
  back/forward use what is in the cache unless the server said to always ask.
- `lowweb.exe --no-cache` uses no cache, `--cache-dir DIR` another folder, `--cookie-file FILE`,
  `--no-http2`, `--sleep-tabs-after SECONDS`; the log (`--log`) has a `[net]` line for every
  request saying whether it came from the cache, a kept or new connection, a resumed TLS session
  or which HTTP/2 stream. (With `--script`, cookies are kept only in memory, apart from the user's.)

**Memory**
- **Tabs not looked at for 10 minutes go to sleep**: the page is unloaded to give back its memory
  (the viewer takes 20–40 MB on a big page), and only a compressed picture of it stays. Coming back
  to the tab loads it again from the cache, at the same place. Only HTML pages loaded with GET and
  not typed into go to sleep (Low-web pages such as Paint have state of their own, so they don't);
  the time is set with `--sleep-tabs-after SECONDS` (`0` = never).
- Back/forward and reload return to the same place in the page (through `lw_state`/`lw_restore`
  in `sdk/lowweb.h`).
- The pictures of background tabs are kept compressed (run-length: 3.2 MB → ~0.3 MB), the viewer
  drops the HTML it has parsed while the page downloads, the viewer's allocator splits and joins
  free blocks, and the text-width cache is a hash table.
- Measured (working set): Wikipedia "United States" 54 → 49 MB, 4 tabs 142 → 124 MB, and 66 MB
  when 3 background tabs are asleep; the script command `mem` writes to the log where the memory
  goes (each tab's wasm, pictures, caches).

**What a page can reach** (`net::Access` in `browser/net.h`)

A page reads every byte `lw_fetch` brings (the viewer decodes pictures and CSS itself), so pages
from the internet must not use the browser to get at what only this computer can reach. Addresses
fall into 3 levels by IP **after DNS resolution** (a name pointing at 127.0.0.1 is caught too):
**this computer** (127.0.0.0/8, ::1), **the local network** (10/8, 172.16/12, 192.168/16,
169.254/16, 100.64/10, fc00::/7, fe80::/10) and **the internet**.

| The page comes from | `lw_fetch` (pictures, CSS, data) | Links (`lw_navigate`) | POST forms |
|---|---|---|---|
| The internet | The internet only | Anywhere but `file://` | The internet only |
| The local network | Local network + internet | Anywhere but `file://` | Local network + internet |
| This computer (`localhost`, `lowd`) | Anywhere but `file://` | Anywhere but `file://` | Anywhere |
| A file (`file://`) | Files in its folder and below + any server | Anywhere | Anywhere |

- Every redirect and every connection is checked: a redirect to 127.0.0.1 or to `file://` is
  blocked too, and kept connections and HTTP cache entries remember which level they came from
  (so a page from the internet can't read a router page that is in the cache).
- File paths are checked once made absolute (`..`, `%2e%2e`, `%5c` can't leave the folder).
- The user can still type any address; what is blocked fails with an error starting with
  `blocked:` and a `[net] blocked …` line in the log.
- This isn't a full same-origin policy yet: a page can still read public data from other sites,
  but it can't send their cookies (third-party cookies are off), so it reads only what anyone
  could download anyway.

Addresses work as on the ordinary web:
- A path ending in `/` first tries `index.wasm` in that folder; if there is none (or it isn't
  wasm), the address itself is requested, so ordinary sites get their usual front page.
- HTML → the viewer, `text/plain` → shown by the browser itself, PNG/JPEG/GIF/BMP pictures → shown
  as they are; `file:///` is for development.

## What the HTML viewer can do

| Part | Supported |
|---|---|
| Charsets | UTF-8, windows-874/TIS-620 (older Thai sites, google.com included), windows-1252/Latin-1 |
| HTML | Tokenizer + tree builder (most of HTML5's implied end tags), character references |
| Layout | Block/inline, line breaking (Thai by words, through Uniscribe), headings, lists, `pre`, tables (colspan, automatic widths), `bgcolor`/`color`/`align`, part of inline `style` |
| Showing pages as they load | The HTML reaches the viewer **while it downloads** (gzip is decoded piece by piece too) and is parsed as it comes; a `<link rel=stylesheet>` is fetched as soon as it is seen; the layout goes top to bottom ~12 ms per frame, the first screen shows as soon as it is laid out, and what hasn't arrived yet follows by itself without freezing the window; on resize, what is in view is laid out first; **a table still loading shows the rows it has** (laid out again as rows come: a page that is one big table, like HN on a slow connection, shows at 1 second instead of at the end); a page with no `<main>` yet waits at most 1 second to decide on reader view, and if `<main>` comes later and the user hasn't scrolled, it switches to reader view by itself |
| CSS | Reads `<style>` and `<link rel=stylesheet>` to find what is **hidden** (`display:none`, `visibility:hidden`, text for screen readers); `@media` by window width; the cascade (specificity, `!important`); each element's CSS is worked out when the layout reaches it, with a Bloom filter of its ancestors to drop selectors that can't match early |
| CSS layout | **Floats** left/right (text flows around them, `clear`, also `<img align>`/`<table align>` and `<br clear>` of older sites; tables beside a float make room); **flex rows** (`flex-grow/shrink/basis`, `flex-wrap`, `justify-content`, `align-items`, `gap`; a row too crowded wraps instead of squeezing its items into slivers); flex columns are laid out as blocks; **grid** (`grid-template-columns`: px/fr/%/`repeat()`/`auto-fill`/`minmax()`, `grid-column: span`); `width`, `max-width`, `min-width`, `display: block/inline`; carousels/strips (`overflow: hidden/auto` around a flex row) show only the items that fit; flex/grid/floats still loading are shown as they are, like tables; a flex row or grid beside a float makes room for it like a table; a right float in the middle of a line (a heading's "more" link) stays on that line; flex `order` and `align-self`; media queries in the range syntax (`(width >= 768px)`, `calc()`); table rows and cells the stylesheet hides take no room |
| CSS as advice | Low-web isn't trying to look like every other browser: CSS is used where it makes a page easier to read. **margin/padding** keep text blocks at least their usual space apart and never more than 3em, no side more than a quarter of the width, negative margins count as 0; a **background colour** is used only where our text stays readable on it (contrast 3:1); **width/max-width** only narrow a block (never under 200 px) and auto margins centre it; `position: fixed/sticky` boxes stay in the flow (no bars stuck over the text), "visually hidden" text for screen readers stays hidden |
| Pictures | PNG, JPEG, GIF, **WebP** (lossy, lossless, alpha, the first frame of animations), **SVG** (`.svg` files and `<svg>` in the page), BMP; `<picture>`/`srcset`; `data:` URIs |
| Reader view | If the site has `<main>`/`role=main`, only that is shown; the badge at the bottom right switches to the full page |
| Forms | text/password/checkbox/radio/select/textarea/submit, sent with GET and POST |
| Video, audio | `<video>` and `<audio controls>` (`src` or `<source>`: the kinds Windows plays come first; `poster`): the picture + a bar of controls (play/pause, time, a seek track that can be dragged, fullscreen, mute); **nothing is downloaded until play is clicked** (no autoplay); keys: Space/K play/pause, ←/→ 5 seconds back/on, M mute, F or a double-click on the picture = fullscreen (Esc to leave) |
| Other | Links, `#anchor`s (also in the address the page is opened with: it scrolls there when the layout gets there; Thai both as characters and as `%E0%B8…`), `<meta refresh>`, scrolling (wheel, keyboard, scrollbar), find in page (`lw_find`) |

Not supported: JavaScript (so videos on YouTube, Facebook, TikTok and Netflix don't play: those
sites fetch video in pieces with JS, and some use DRM; HLS/DASH aren't there yet either), CSS
`position` (absolute/fixed/sticky are laid out in normal flow), margin/padding/background colours
from stylesheets (only in `style=""`), real inline-block, AVIF.
(So sites that need JS, like Google's search results, don't work; DuckDuckGo Lite, Wikipedia,
Hacker News and BBC do.)

## Write your own page

```c
#include "lowweb.h"
static unsigned fb[640 * 360];

LW_EXPORT(lw_start) void lw_start(void) { lw_set_title(LW_STR("Hello")); }

LW_EXPORT(lw_frame) void lw_frame(double ms) {
    for (int i = 0; i < 640 * 360; i++) fb[i] = 0xFF3B2A1E;          // 0xAABBGGRR
    lw_text(fb, 640, 360, 40, 40, LW_STR("Hello, Low-web! สวัสดี"), 40, LW_TEXT_BOLD, 0xFFFFFFFF);
    lw_present(fb, 640, 360);
}
```

```bat
clang --target=wasm32 -O2 -mcpu=mvp -mbulk-memory -mnontrapping-fptoint -msign-ext ^
      -mmutable-globals -nostdlib -fno-builtin -Wl,--no-entry -Isdk -o index.wasm hello.c
```

Every function is in [`sdk/lowweb.h`](sdk/lowweb.h). In short:

| The page exports (the browser calls) | The browser provides (the page calls) |
|---|---|
| `lw_start`, `lw_resize`, `lw_frame` | `lw_present` puts a frame on the screen |
| `lw_pointer` (the mouse → returns a cursor) | `lw_text`, `lw_text_width` (bold/italic/monospace), `lw_line_breaks` |
| `lw_key`, `lw_char` | `lw_fetch` → the answer comes through `lw_on_fetch` / `lw_on_fetch_ex` |
| `lw_alloc` (for the browser to get memory in the page) | `lw_open_file` → `lw_on_file`, `lw_save_file`, `lw_clipboard_set/get` |
| `lw_on_file`, `lw_on_fetch`, `lw_on_fetch_ex`, `lw_on_fetch_begin/data/end` (a streamed document), `lw_state`/`lw_restore` (where the reader is), `lw_find` (find in page), `lw_on_menu` (an item of the page's own in the right-click menu was picked) (the last four are optional) | `lw_image_decode/read/free`, `lw_navigate`, `lw_navigate_post`, `lw_open_tab`, `lw_mods`, `lw_set_title`, `lw_now`, `lw_scale`, `lw_log` |
| | `lw_video_open/play/pause/seek/volume/info/error/close`: video and audio; the page says where each goes with `lw_video_place` at every `lw_present` and the browser draws the pictures there itself (not through the interpreter, frame by frame); sound can start only while the user clicks or presses a key (otherwise it plays muted) |
| | `lw_fullscreen` (only while the user clicks or presses a key; the user can leave with Esc/F11) |
| | `lw_menu` (while handling a right click: the page's own items for the menu the browser shows, above Back/Forward/Reload) |

## Hosting

A Low-web site is only static files (`.wasm`, `.txt`, pictures), so there are 3 ways to host one:

1. **`lowd` on a VPS / Linux**: `g++ -std=c++17 -O2 -pthread -o lowd server/lowd.cpp`, then
   `./lowd -p 80 www` (or another port behind a reverse proxy); users open `http://your-domain/`.
2. **Your own computer**: `bin\lowd.exe sites\www` (listens on every interface, IPv4 + IPv6), allow
   it in Windows Firewall, then forward port 8080 on the router, or use a tunnel such as
   Cloudflare Tunnel if there is no public IP.
3. **A static host with HTTPS** (GitHub Pages, Cloudflare Pages, Netlify …): upload the `www`
   folder as it is; the browser handles HTTPS and redirects itself, it only needs an `index.wasm`
   in each folder.

## Testing

Every push and pull request on GitHub is built and tested (`.github/workflows/ci.yml`): `build.cmd`
+ `tests/run_tests.py` + `tests/check_lowd.sh` on Windows, and `lowd` + `check_lowd.sh` on Ubuntu;
`lowweb.exe` and `lowd.exe` can be downloaded as artifacts (kept 14 days), and compiler warnings
show as annotations.

| Command | What it tests |
|---|---|
| `python tests/run_tests.py [NAME…]` | **Every test that needs no internet**, in one command (ops, viewer, images, stream, cache, cookies, hpack, access, sleep, select, edit, progressive, css, video, history, referer), as CI runs them: run it before committing |
| `python tests/check_referer.py` | The headers the browser sends (a test server on 2 ports = 2 origins): a Chrome-style User-Agent + `Low-web/0.1`; Referer as browsers send it by default (`strict-origin-when-cross-origin`): the whole address (no #fragment) to the same origin, only the origin to another one, none for a typed address |
| `python tests/check_history.py` | Back/forward: a link the user clicks adds an entry, but a page that moves on by itself (a `<meta refresh>` like a search engine's redirect page) takes its own place, so Back goes back to the search instead of round to the same page |
| `python tests/check_video.py` | Video (`tests/samples/colors.mp4`: one second each of red, green, blue and yellow, so a screenshot tells where it is): click to play, Space to pause, a click on the track at 85%, playing to the end, F for fullscreen / Esc, `<audio>`, a missing file says why on its picture, and over slow HTTP (`slow_server.py`) with the picture data 3 MB into the file, which needs a range request; SKIP where Windows has no Media Foundation, and the `<audio>` check is skipped on a computer with no sound device |
| `python tests/check_css_layout.py` | CSS layout from stylesheets: a page of coloured boxes (a flex row 1:2 + a fixed width + gap, a right float with text around it + clear, a 3-column grid, a carousel in `overflow:hidden`, `justify-content:center`, a float with `clear` that goes below the float before it while the text after it stays up beside that one, a "more" float on a heading's line, range media queries, a hidden table cell, GitHub's visually hidden headings and empty `:is()`, margin/padding/background as advice, a centred max-width column, flex `order`/`align-self`), each box found by its colour in a screenshot |
| `python tests/check_menu.py` | The right-click menu: the items on a link, an image and plain text (Copy greyed out until something is selected), the browser's own after them, and what picking does (copy a link's whole address, save an image's file, copy the selection) |
| `python tests/check_progressive.py` | Pages loading slowly (`slow_server.py` on a free port): a big table shows rows before the download ends, a page without `<main>` shows its first screen before the end, a `<main>` that comes late still ends in reader view, and the final picture is the same as when loaded at once |
| `python tests/check_edit.py` | Text fields (typing, arrows, Home/End, Shift to select, Delete/Backspace, cutting, Thai vowels, double-click, drag to select, Tab, textarea), Thai `#fragment`s as characters and as `%`, find in page (count, next/previous, case, not found, text files) |
| `python tests/check_select.py` | Selecting/copying/pasting text: the whole page, a drag (across a link), text fields (paste, cut, password fields can't be copied), text files, error pages; the tab strip: which parts are caption/buttons/resize edge, the maximize button |
| `python tests/check_sleep.py` | A tab that went to sleep wakes up the same to the pixel at the same place, back and reload return to the same place, pages typed into and Low-web pages don't sleep |
| `python tests/check_access.py` | What pages can reach: IP levels, pages from the internet/local network can't reach 127.0.0.1, `localhost`, `[::1]`, redirects to `file://`, cache entries, files outside the folder (`..`, `%2e%2e`, `%5c`) |
| `bash tests/check_lowd.sh [LOWD]` | `lowd`: serves `index.wasm`, redirects folders, 404, blocks `..`/`%2e%2e`, refuses POST, HEAD |
| `node tests/check_ops.mjs` against `bin\wasmrun build\ops.wasm "int32()" "int64()" "floats()" "control()" "memory_ops()"` | The interpreter gives the same results as V8 to the bit (integers, floats, control flow, memory, traps); `wasmrun --no-fuse` runs without fused instructions and must give the same |
| `python tests/check_images.py` | The decoders match PIL (every kind of PNG, JPEG baseline/progressive/subsampling/restart/EXIF, GIF, BMP) and **WebP matches libwebp to the pixel** |
| `python tests/check_cookies.py` | Cookies: every rule of the jar (`bin\cookietest`: domain, path, Secure, lifetime, SameSite, third-party, prefixes, saving to the file) and over real HTTP: cookies from a redirect reach the next page, a cached `Vary: Cookie` answer isn't reused when the cookies change |
| `python tests/check_hpack.py` | HTTP/2's HPACK matches every example in RFC 7541 Appendix C (with/without Huffman, a full dynamic table and evictions), the static table of Appendix A, and encoding then decoding gives the same back |
| `bin\fetchtest --parallel [--no-h2] URL…` | Fetches many URLs at once: HTTP/2 uses one connection; `--save DIR` keeps the bodies to compare with HTTP/1.1 |
| `python tests/bench_viewer.py` | Times the viewer on a 3 MB Wikipedia page without a window (`bin\viewerbench`), with and without instruction fusion, and checks that both draw the same final picture to the pixel; `viewerbench --stream N` hands the HTML over N bytes at a time as a download does, and prints the size of wasm memory |
| `python tests/check_cache.py` | HTTP cache and keep-alive with a local test server: `max-age`, `Expires`, `ETag`/`If-None-Match`, `Last-Modified`, `no-store`, cached redirects, reload/hard reload, many requests on one connection (chunked too), `Connection: close`, a connection the server closed that needs the request sent again |
| `python tests/check_stream.py` | gzip/zlib/deflate decoded piece by piece (as for showing pages while they download), from pieces of random size, must give the original bytes |
| `bin\fetchtest --stream URL…` | Receives bodies piece by piece from the real network and checks they match the whole body |
| `python tests/check_svg.py` | SVG next to Microsoft Edge (headless), side by side, in `build/svgtest/*.cmp.png` |
| `python tests/gen_samples.py` | Makes the sample pictures `tests/samples/gen_*` |
| `python tests/gen_hpack_huffman.py` | Makes `browser/hpack_huffman.h` (HPACK's Huffman table, from RFC 7541) |
| `python tests/gen_webp_tables.py` | Makes `browser/webp_tables.h` (the constant tables of VP8/VP8L, from the spec) |
| `bin\fetchtest [--exact] [--cache DIR] URL…` | HTTP/HTTPS, redirects, chunked, gzip, certificate checks, the `index.wasm` rule; the log says whether each request came from the cache or a kept connection |
| `bin\lowweb.exe URL --size 1000x680 --log out.log --script "wait 500; click 100 200; key 83 ctrl" --screenshot out.bmp` | Drives the browser and takes a screenshot (the log gives times from the start of the load, e.g. `[page +383 ms] viewer: first screen…`; waiting commands wait for the load to finish unless `async` was given (back with `sync`); `mem` writes memory use to the log; `clip TEXT` puts text on the script's clipboard (script mode doesn't touch the real clipboard: every copy is logged as `[clipboard] …`); `hittest X Y` says what that point of the tab strip is; `chars TEXT` types into the page; `find TEXT` / `findnext` / `findprev` / `findclose` use the find bar (logged as `[find] 3/12`); `video` logs what the page's videos are doing, `waitvideo ID STATE [MS]` waits until one is in that state; `history` logs the tab's history) |

## Known limitations

- The browser runs on Windows (Win32 + SChannel); `lowd` is also built and tested on Linux in CI.
- Pages run on the UI thread in an interpreter (frequent instruction sequences are fused when a
  module loads, plus threaded dispatch: ~2× faster than before; no JIT yet). Heavy work (such as a
  flood fill of the whole canvas in Paint) takes ~0.1 s; a 3 MB Wikipedia page: parsing ~0.12 s,
  first layout ~0.29 s (again when pictures arrive ~0.08 s), first screen ~0.4 s after the first
  Enter and ~0.2 s with the CSS in the cache. A Low-web page stuck for more than 5 seconds (the
  viewer: 20 seconds) is stopped.
- No TLS 1.3 yet (Windows 10's SChannel doesn't offer TLS 1.3 to clients, so TLS 1.2 is used).
- Video plays through Windows' Media Foundation: Windows "N" editions need the Media Feature Pack,
  computers without a sound device (such as servers) play videos without their sound but can't
  play audio alone, and MP4 files with B-frames show the picture a few frames behind the sound
  (Media Foundation ignores edit lists).
  CPU while playing: H.264 640×360 ~5% of one core, Wikipedia's MPEG-4 Part 2 ~14% (decoded on
  the CPU).
- CSS not yet used: `display: inline-block` with blocks inside (it is laid out inline), `:is()`/`:not()`
  with combinators inside, `color`/`font-size` from stylesheets, `position: absolute` placement.
  Reading margin/padding/background costs ~10% of the layout of a 3 MB Wikipedia page.
- Pages from the internet are kept away from this computer, the local network and files (see
  "What a page can reach"), but there is no full same-origin policy yet.
