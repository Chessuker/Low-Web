// lowweb.h — the Low-web page ABI, version 1.
//
// A Low-web page is a single WebAssembly module (index.wasm). There is no HTML,
// CSS or JavaScript: the page owns every pixel it shows and talks to the browser
// only through the functions below.
//
// Build a page (C, no libc):
//   clang --target=wasm32 -O2 -mcpu=mvp -mbulk-memory -mnontrapping-fptoint -msign-ext
//         -mmutable-globals -nostdlib -fno-builtin -Wl,--no-entry -Isdk -o index.wasm page.c
//
// -fno-builtin stops clang from turning loops into calls to libc functions
// (strlen, memset...) that do not exist here; __builtin_memcpy/__builtin_memset
// still work and become single wasm instructions (memory.copy / memory.fill).
//
// Lifecycle (all calls happen on the browser's UI thread, never re-entrantly):
//   lw_start()                  once, after the module is instantiated        (required)
//   lw_resize(w, h)             after start and whenever the view changes size (optional)
//   lw_frame(ms)                about 60 times a second                       (optional)
//   lw_pointer(kind, x, y, b)   mouse input, in the coordinates of the last presented frame
//   lw_key(key, mods, down)     key presses: key is 'A'..'Z', '0'..'9' or an LW_KEY_* code
//   lw_char(codepoint)          text input (Unicode, printable characters only)
//   lw_on_file(...)             a file the user picked (lw_open_file) or dropped on the page
//   lw_on_fetch(...)            the answer to an lw_fetch request
//
// When the browser hands the page bytes (files, fetch results) it first calls
// lw_alloc(n) and writes into the returned memory. The page owns that memory afterwards.
//
// Export signatures (define the ones you need with LW_EXPORT):
//   void  lw_start(void);
//   void  lw_resize(int w, int h);                        view size in device pixels
//   void  lw_frame(double ms);                            ms = lw_now()
//   int   lw_pointer(int kind, float x, float y, int b);  returns an LW_CURSOR_*
//   int   lw_key(int key, int mods, int down);            returns 1 if handled
//   void  lw_char(int codepoint);
//   void *lw_alloc(int n);                                return 0 to refuse
//   void  lw_on_file(const void *data, int len, const char *name, int name_len);
//   void  lw_on_fetch(int id, int status, const void *data, int len);
//         status is the HTTP status, or 0 if the request failed (data = error message)
//   void  lw_on_fetch_ex(int id, int status, const void *data, int len,
//                        const char *type, int type_len, const char *url, int url_len);
//         like lw_on_fetch plus the Content-Type and the final URL after redirects;
//         used instead of lw_on_fetch when the page exports it
//   void  lw_on_fetch_begin(int id, int status, const char *type, int type_len,
//                           const char *url, int url_len);
//   void  lw_on_fetch_data(int id, void *data, int len);
//   void  lw_on_fetch_end(int id, int status);
//         a body delivered while it downloads: begin (headers are in; type and url
//         share one block from lw_alloc that the page frees through `type`), data for
//         each piece (decompressed; the page owns it), end (status 0 = cut off).
//         Only for the document the viewer shows (id 0), and only if all three exist.
//   int   lw_state(void);
//   void  lw_restore(int state);
//         optional: where the user is in the page (the viewer: scroll position). The browser
//         asks before it unloads the page (back/forward, reload; for the viewer also a
//         background tab put to sleep to save memory) and hands the number back right
//         after lw_start when the same history entry is loaded again. 0 = nothing to restore.
//
// Documents the browser cannot show itself (HTML) are opened with a built-in viewer page
// (sites/viewer/viewer.c); it receives the document as fetch id 0, streamed through
// lw_on_fetch_begin/data/end (or whole through lw_on_fetch_ex).

#ifndef LOWWEB_H
#define LOWWEB_H

#define LW_IMPORT(name) __attribute__((import_module("lw"), import_name(#name)))
#define LW_EXPORT(name) __attribute__((export_name(#name)))

// ---- pointer kinds (lw_pointer) ---------------------------------------------
enum { LW_DOWN = 0, LW_MOVE = 1, LW_UP = 2, LW_LEAVE = 3, LW_WHEEL = 4 };
// For DOWN/UP the last argument is the button (0 left, 1 middle, 2 right);
// for WHEEL it is the wheel delta (120 per notch, positive = away from the user).

// ---- cursors (return value of lw_pointer) -----------------------------------
enum { LW_CURSOR_ARROW = 0, LW_CURSOR_HAND = 1, LW_CURSOR_NONE = 2,
       LW_CURSOR_CROSS = 3, LW_CURSOR_TEXT = 4 };

// ---- modifier bits (lw_key) -------------------------------------------------
enum { LW_SHIFT = 1, LW_CTRL = 2, LW_ALT = 4 };

// ---- special keys (lw_key) --------------------------------------------------
enum {
    LW_KEY_BACKSPACE = 8, LW_KEY_TAB = 9, LW_KEY_ENTER = 13, LW_KEY_ESCAPE = 27,
    LW_KEY_SPACE = 32, LW_KEY_PAGEUP = 33, LW_KEY_PAGEDOWN = 34, LW_KEY_END = 35,
    LW_KEY_HOME = 36, LW_KEY_LEFT = 37, LW_KEY_UP = 38, LW_KEY_RIGHT = 39,
    LW_KEY_DOWN = 40, LW_KEY_INSERT = 45, LW_KEY_DELETE = 46,
    LW_KEY_F1 = 112, // F1..F12 = 112..123
    LW_KEY_LBRACKET = 219, LW_KEY_RBRACKET = 221,
};

// ---- text flags (lw_text) ---------------------------------------------------
enum { LW_TEXT_BOLD = 1, LW_TEXT_ITALIC = 2, LW_TEXT_MONO = 4 };

// ---- imports: what the browser provides -------------------------------------

// Show a frame. rgba: w*h pixels, 4 bytes each (R,G,B,A in memory order).
// The browser copies the pixels, so the page may reuse the buffer immediately.
LW_IMPORT(present) void lw_present(const void *rgba, int w, int h);

// Tab/window title (UTF-8).
LW_IMPORT(set_title) void lw_set_title(const char *s, int len);

// Debug output (UTF-8), shown in the terminal the browser was started from.
LW_IMPORT(log) void lw_log(const char *s, int len);

// Milliseconds since the page started.
LW_IMPORT(now) double lw_now(void);

// Device pixels per logical pixel (1.0 at 96 DPI, 1.5 at 144 DPI...). lw_resize and
// lw_pointer use device pixels; multiply your sizes by this to look right on any screen.
LW_IMPORT(scale) double lw_scale(void);

// Go to another page. Relative URLs ("paint/", "../x/", "/") resolve against this page.
LW_IMPORT(navigate) void lw_navigate(const char *url, int len);

// Go to another page by POSTing a form (body: application/x-www-form-urlencoded).
LW_IMPORT(navigate_post) void lw_navigate_post(const char *url, int len, const char *body, int body_len);

// Open a URL in a new tab; foreground = 1 switches to it, 0 opens it in the background.
LW_IMPORT(open_tab) void lw_open_tab(const char *url, int len, int foreground);

// The modifier keys held right now (LW_SHIFT | LW_CTRL | LW_ALT), e.g. for Ctrl+click.
LW_IMPORT(mods) int lw_mods(void);

// Ask the user for a file. accept: extensions separated by ';', e.g. "png;jpg"
// (empty = any file). The result arrives later through lw_on_file.
LW_IMPORT(open_file) void lw_open_file(const char *accept, int len);

// Offer bytes to the user as a download. The browser copies the data immediately.
LW_IMPORT(save_file) void lw_save_file(const char *name, int name_len, const void *data, int len);

// Start an HTTP(S) GET of exactly this URL. Returns a request id; the answer arrives
// through lw_on_fetch (or lw_on_fetch_ex).
LW_IMPORT(fetch) int lw_fetch(const char *url, int len);

// Decode a PNG, JPEG or BMP image. Returns a handle (0 = not an image we can read)
// and writes the size to *w, *h. Then allocate w*h*4 bytes and call lw_image_read,
// which copies RGBA pixels out and frees the handle (or lw_image_free to drop it).
LW_IMPORT(image_decode) int lw_image_decode(const void *data, int len, int *w, int *h);
LW_IMPORT(image_read) void lw_image_read(int handle, void *rgba);
LW_IMPORT(image_free) void lw_image_free(int handle);

// Draw UTF-8 text into an RGBA buffer using the system's font engine (any script,
// including Thai). y is the top of the line; rgba is the colour as 0xAABBGGRR.
// Returns the width in pixels. lw_text_width only measures.
LW_IMPORT(text) int lw_text(void *fb, int fb_w, int fb_h, int x, int y,
                            const char *s, int len, int size_px, int flags, unsigned rgba);
LW_IMPORT(text_width) int lw_text_width(const char *s, int len, int size_px, int flags);

// Where lines may break in UTF-8 text, using the system's word-break rules (so Thai,
// which has no spaces, breaks between words). Sets out[i] = 1 if a line may break before
// byte i, else 0. `out` must hold len bytes.
LW_IMPORT(line_breaks) void lw_line_breaks(const char *s, int len, unsigned char *out);

// The clipboard, as UTF-8 text. lw_clipboard_set works while the page handles the user's
// input (a key, a click, typing): pages can't overwrite the clipboard on their own.
// lw_clipboard_get works only while the page handles a paste key (Ctrl+V, Shift+Insert),
// so a page can't read the clipboard behind the user's back. It copies at most cap bytes
// to buf and returns the whole text's length (call again with a bigger buffer if it is
// more than cap), or -1 if there is no text or reading isn't allowed now.
LW_IMPORT(clipboard_set) void lw_clipboard_set(const char *s, int len);
LW_IMPORT(clipboard_get) int lw_clipboard_get(char *buf, int cap);

// ---- helpers ----------------------------------------------------------------

static inline int lw_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
#define LW_STR(s) (s), lw_strlen(s)

#endif
