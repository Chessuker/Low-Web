# Low-web

[![CI](https://github.com/Chessuker/Low-Web/actions/workflows/ci.yml/badge.svg)](https://github.com/Chessuker/Low-Web/actions/workflows/ci.yml)

เว็บที่ไม่มี HTML, CSS หรือ JavaScript และ browser ที่เขียนขึ้นเองสำหรับมัน

หน้าเว็บของ Low-web คือโปรแกรม WebAssembly หนึ่งไฟล์ (`index.wasm`) ที่วาดพิกเซลเองทั้งหมด
ส่วน browser (C++ ล้วน ไม่มี framework) ทำหน้าที่เหมือน "เครื่อง" ให้โปรแกรมนั้นรัน:
ดาวน์โหลดผ่าน HTTP/HTTPS, รันด้วย interpreter ของตัวเอง, แล้วเอาพิกเซลที่ได้ขึ้นจอ

เว็บ HTML ปกติก็เปิดได้ด้วย: browser ส่งเอกสาร HTML ให้ **viewer** ซึ่งเป็นหน้า Low-web
อีกหน้าหนึ่ง (เขียนด้วย C) ที่อ่าน HTML แล้ววาดออกมาแบบ reader view ตัว browser เองจึงยังไม่รู้จัก HTML

```
 ┌──────────── lowweb.exe (C++, Win32) ─────────────┐        ┌──── lowd (C++) ────┐
 │ address bar ─► net.cpp  HTTP/1.1, HTTP/2 + TLS    ├──TCP──►│ static files       │
 │                 │        gzip (inflate.cpp)       │        │ /  → /index.wasm   │
 │                 ▼                                 │        └────────────────────┘
 │   index.wasm? ──► รันหน้านั้นเลย                                                 │
 │   HTML?  ───────► viewer.wasm (ฝังอยู่ใน exe) + เอกสาร HTML                      │
 │                                                                                 │
 │ wasm.cpp  WebAssembly interpreter  ◄── หน้าเว็บ (C → wasm32)                    │
 │   │  imports "lw.*"  (sdk/lowweb.h)                                             │
 │   ├─ lw_present ──► framebuffer ─► StretchDIBits (จอ)                           │
 │   ├─ lw_text ─────► GDI + Uniscribe (ทุกภาษา รวมถึงไทย ตัดคำไทยถูก)              │
 │   ├─ lw_image_decode ► PNG / JPEG / GIF / WebP / SVG / BMP (เขียนเองทั้งหมด)      │
 │   └─ lw_fetch / lw_navigate(_post) / lw_open_file / lw_save_file               │
 └──────────────────────────────────────────────────┘
```

## โครงสร้าง

| โฟลเดอร์ | อะไร |
|---|---|
| `browser/` | ตัว browser: `main.cpp` (หน้าต่าง, แท็บ, address bar, host ของ page ABI), `wasm.cpp` (interpreter), `net.cpp` (URL, HTTP/1.1 + keep-alive), `http2.cpp` (HTTP/2 + HPACK), `conn.h` (TCP + TLS ผ่าน SChannel), `cache.cpp` (HTTP cache), `cookies.cpp` (cookie), `inflate.cpp` (DEFLATE/gzip), `image.cpp` (PNG/JPEG/GIF/BMP), `webp.cpp` (WebP lossy + lossless), `svg.cpp` (SVG renderer) |
| `server/lowd.cpp` | web server สำหรับ host เว็บ Low-web (Windows + Linux) |
| `sdk/lowweb.h` | **Page ABI**: สัญญาระหว่างหน้าเว็บกับ browser |
| `sdk/hello.c` | หน้าเว็บตัวอย่างที่เล็กที่สุด (~1 KB) ใช้เป็นแม่แบบ |
| `sites/home/`, `sites/paint/` | source ของหน้าเว็บตัวอย่าง |
| `sites/viewer/` | **HTML viewer**: `viewer.c` (HTML → layout → วาด) และ `css.c` (CSS เท่าที่จำเป็น) |
| `sites/www/` | **web root** ที่ host จริง (`index.wasm`, `paint/index.wasm`, `motd.txt`, `about.txt`) |
| `tests/` | ชุดทดสอบ interpreter / decoder / network / cache, ไฟล์ตัวอย่างจากเว็บจริง (`tests/samples/`) และสคริปต์ช่วยพัฒนา (`dev.sh`, `shot.sh`, `slow_server.py`) |
| `build/` | ของที่ build สร้าง (`viewer.wasm`, `lowweb_res.o`, `lowd.exe`, `ops.wasm`) และผลทดสอบชั่วคราว ลบทิ้งได้ทั้งโฟลเดอร์ (`source-backups.zip` คือ source ก่อนแก้รอบใหญ่ ๆ เก็บไว้เผื่อย้อนดู) |
| `legacy/paint-html/` | paint เวอร์ชันเดิมที่ยังพึ่ง HTML + JS |

## Build

ต้องมี **g++ (MinGW-w64)** และ **clang ที่มี target wasm32** (LLVM)

```bat
build.cmd
```

ได้ `bin\lowweb.exe` (browser ที่ฝัง viewer ไว้แล้ว), `bin\lowd.exe` (server), `sites\www\**\index.wasm`
และเครื่องมือทดสอบ ถ้า `lowd.exe` กำลังรันอยู่ build จะไม่เขียนทับ (ตัวใหม่อยู่ที่ `build\lowd.exe`)

ระหว่างพัฒนา viewer: วาง `viewer.wasm` ไว้ข้าง `lowweb.exe` แล้ว browser จะใช้ไฟล์นั้นแทนตัวที่ฝังไว้
ไม่ต้อง build exe ใหม่ (`bash tests/dev.sh` ทำให้; `bash tests/shot.sh NAME URL` เปิดหน้าแล้วเก็บภาพเป็น PNG ที่ `build/shots/`)

## ใช้งาน

```bat
bin\lowweb.exe                         :: เปิดเว็บตัวอย่างจากดิสก์ (sites\www)
bin\lowd.exe sites\www                 :: เปิด server ที่พอร์ต 8080
bin\lowweb.exe http://localhost:8080/  :: เปิดผ่าน network จริง
bin\lowweb.exe https://th.wikipedia.org/wiki/ภาษาไทย   :: เว็บ HTML ปกติ (reader view)
```

คีย์ลัด:

| คีย์ | ทำอะไร |
|---|---|
| `Ctrl+L` | ไปที่ช่อง address (พิมพ์ URL หรือคำค้นหาก็ได้) |
| `Ctrl+T` / `Ctrl+W` | เปิด / ปิดแท็บ |
| `Ctrl+Tab`, `Ctrl+Shift+Tab`, `Ctrl+1`…`9` | สลับแท็บ |
| `Ctrl+Shift+T` | เปิดแท็บที่เพิ่งปิดกลับมา |
| `Alt+Enter` ในช่อง address | เปิดในแท็บใหม่ |
| คลิกกลาง / `Ctrl`+คลิกลิงก์ | เปิดลิงก์ในแท็บเบื้องหลัง |
| `F5` · `Ctrl+F5` · `Alt+←/→` · `Alt+Home` | reload (ถาม server ว่าเปลี่ยนไหม) · reload โดยไม่ใช้ cache · back/forward · home |

ลากไฟล์ `.wasm` มาวางเพื่อรัน · ลากรูปมาวางบน Paint เพื่อเปิดรูป

**เลือก คัดลอก วางข้อความ**: ลากเมาส์เพื่อเลือก, ดับเบิลคลิก = คำ, คลิกสามครั้ง = ทั้งย่อหน้า, `Shift`+คลิก = ขยายส่วนที่เลือก,
`Ctrl+A` เลือกทั้งหน้า, `Ctrl+C` คัดลอก (ขึ้นบรรทัดใหม่ตามหน้า), ลากเลยขอบบน/ล่างแล้วหน้าเลื่อนตาม; ในช่องกรอก `Ctrl+V` วาง,
`Ctrl+A` แล้ว `Ctrl+C`/`Ctrl+X` คัดลอก/ตัด (ช่องรหัสผ่านคัดลอกไม่ได้); หน้าที่ browser แสดงเอง (ไฟล์ข้อความ, หน้า error) ก็เลือกและคัดลอกได้
หน้า Low-web ใช้ clipboard ผ่าน `lw_clipboard_set`/`lw_clipboard_get`: เขียนได้เฉพาะตอนจัดการ input ของผู้ใช้
และอ่านได้เฉพาะตอนผู้ใช้กด `Ctrl+V`/`Shift+Insert` เท่านั้น หน้าเว็บจึงแอบอ่านหรือเขียนทับ clipboard ไม่ได้

**แถบแท็บคือ title bar**: หน้าต่างไม่มี title bar ของ Windows แล้ว แถบแท็บอยู่บนสุดแทนแบบ browser ทั่วไป
มีปุ่มย่อ/ขยาย/ปิดของตัวเองทางขวา, ลากที่ว่างบนแถบเพื่อย้ายหน้าต่าง, ดับเบิลคลิกที่ว่างเพื่อขยายเต็มจอ, คลิกขวาที่ว่างได้เมนูของหน้าต่าง,
ลากขอบบนเพื่อปรับขนาด; Aero Snap และ `Win+←/→/↑` ใช้ได้ตามปกติ

**ค้นหาจากช่อง address**: ถ้าสิ่งที่พิมพ์ไม่ใช่ address (มีช่องว่าง, ไม่มีจุด, ขึ้นต้นด้วย `?`) จะค้นหาแทน
ปุ่มเล็กข้างช่อง address ใช้เลือก search engine: **DuckDuckGo** (ค่าเริ่มต้น), **Google**, **Bing**
ค่าที่เลือกเก็บไว้ที่ `%APPDATA%\Low-web\settings.ini`
(Google ต้องใช้ JavaScript กับหน้าผลค้นหาแล้ว จึงจะได้หน้า "เปิด JavaScript" แทนผลลัพธ์)

**Network, cache และ cookie**
- **HTTP/2** (RFC 9113) สำหรับ HTTPS ที่ server รองรับ (ตกลงกันผ่าน ALPN ตอน TLS handshake): ทุก request ไปที่ server เดียวกัน
  ใช้ connection เดียว (หน้า Wikipedia: 3 connection แทน 10), HPACK เขียนเองรวม Huffman (ตารางสร้างจาก RFC 7541 ด้วย
  `tests/gen_hpack_huffman.py`); server ที่ตอบเป็น HTTP/1.1 จะถูกจำไว้ ไม่ต้องลองใหม่ทุกครั้ง
- **Keep-alive** (HTTP/1.1): connection ที่ใช้เสร็จจะถูกเก็บไว้ใช้กับ request ถัดไปที่ server เดียวกัน (สูงสุด 16 เส้น นาน 60 วินาที)
  ไม่ต้อง handshake TCP + TLS ใหม่ทุกครั้ง; ถ้า server ปิด connection ที่เก็บไว้ไปแล้ว จะส่ง request ใหม่บน connection ใหม่ให้เอง
- **TLS session resumption**: connection ใหม่ไปยัง server ที่เคยต่อแล้วใช้ handshake แบบย่อ (Wikipedia: 78 → 31 ms)
- **Cookie** (RFC 6265): `Domain`, `Path`, `Secure`, `HttpOnly`, `Max-Age`/`Expires`, `SameSite` (ไม่บอก = `Lax`),
  prefix `__Host-`/`__Secure-`, ห้ามตั้งให้ public suffix (`.com`, `.co.th` …); **cookie ของเว็บอื่น (third-party) ไม่ส่งและไม่เก็บ**;
  cookie ที่มีวันหมดอายุเก็บไว้ที่ `%LOCALAPPDATA%\Low-web\cookies.txt` ส่วน session cookie หายเมื่อปิด browser
- **HTTP cache**: ทำตาม `Cache-Control` (`max-age`, `no-cache`, `no-store`), `Expires`, `ETag`/`If-None-Match`,
  `Last-Modified`/`If-Modified-Since` (ถ้า server ไม่บอกอายุ ใช้กฎ 10% ของ `Last-Modified` ไม่เกิน 1 วัน)
  เก็บในหน่วยความจำ 16 MB (ไฟล์ละไม่เกิน 2 MB) และในดิสก์ที่ `%LOCALAPPDATA%\Low-web\Cache` ไม่เกิน 256 MB (ไฟล์ที่ไม่ได้ใช้นานที่สุดถูกลบก่อน)
- `F5` ถาม server ว่าหน้าเปลี่ยนไหม, `Ctrl+F5` / `Ctrl+Shift+R` โหลดใหม่ทั้งหมด, back/forward ใช้ของใน cache ถ้า server ไม่ได้สั่งให้ถามทุกครั้ง
- `lowweb.exe --no-cache` ไม่ใช้ cache, `--cache-dir DIR` ใช้โฟลเดอร์อื่น, `--cookie-file FILE`, `--no-http2`, `--sleep-tabs-after SECONDS`;
  log (`--log`) มีบรรทัด `[net]` บอกทุก request ว่ามาจาก cache, connection เดิม/ใหม่, TLS แบบย่อ หรือ HTTP/2 stream ไหน
  (ตอนรันด้วย `--script` cookie อยู่ในหน่วยความจำเท่านั้น ไม่ปนกับของผู้ใช้)

**หน่วยความจำ**
- **แท็บที่ไม่ได้ดูนาน 10 นาทีจะ "หลับ"**: หน้าถูกปิดเพื่อคืนหน่วยความจำ (viewer บนหน้าใหญ่ใช้ 20–40 MB) เหลือแค่ภาพย่อที่บีบอัดไว้
  พอกลับมาที่แท็บจะโหลดใหม่จาก cache และกลับไปที่ตำแหน่งเดิม; หลับเฉพาะหน้า HTML ที่โหลดด้วย GET และยังไม่ได้พิมพ์อะไรลงไป
  (หน้า Low-web อย่าง Paint มี state ของตัวเอง จึงไม่หลับ); ตั้งเวลาได้ด้วย `--sleep-tabs-after SECONDS` (`0` = ไม่หลับเลย)
- back/forward และ reload กลับไปที่ตำแหน่งเดิมในหน้า (ส่งผ่าน `lw_state`/`lw_restore` ใน `sdk/lowweb.h`)
- ภาพของแท็บเบื้องหลังเก็บแบบบีบอัด (run-length: 3.2 MB → ~0.3 MB), viewer ทิ้ง HTML ส่วนที่ parse แล้วระหว่างดาวน์โหลด,
  allocator ของ viewer แบ่ง/รวมก้อนที่ว่าง, cache ความกว้างข้อความเก็บเป็น hash
- วัดได้ (working set): Wikipedia "United States" 54 → 49 MB, 4 แท็บ 142 → 124 MB และ 66 MB เมื่อ 3 แท็บเบื้องหลังหลับ;
  script command `mem` เขียนลง log ว่าหน่วยความจำไปอยู่ที่ไหน (wasm ของแต่ละแท็บ, ภาพ, cache)

**หน้าเว็บเข้าถึงอะไรได้บ้าง** (`net::Access` ใน `browser/net.h`)

หน้าเว็บอ่านผลของ `lw_fetch` ได้ทุกไบต์ (viewer ต้องถอดรูปและ CSS เอง) จึงต้องกันไม่ให้หน้าเว็บจากอินเทอร์เน็ต
ใช้ browser เป็นทางเข้าไปหาของที่มีแค่เครื่องเราเข้าถึงได้ แบ่ง address เป็น 3 ระดับตาม IP **หลัง resolve DNS แล้ว**
(ชื่อที่ชี้มา 127.0.0.1 ก็โดนกัน): **เครื่องนี้** (127.0.0.0/8, ::1), **วงแลน** (10/8, 172.16/12, 192.168/16, 169.254/16,
100.64/10, fc00::/7, fe80::/10) และ **อินเทอร์เน็ต**

| หน้าเว็บมาจาก | `lw_fetch` (รูป, CSS, ข้อมูล) | ลิงก์ (`lw_navigate`) | ส่งฟอร์ม POST |
|---|---|---|---|
| อินเทอร์เน็ต | อินเทอร์เน็ตเท่านั้น | ที่ไหนก็ได้ ยกเว้น `file://` | อินเทอร์เน็ตเท่านั้น |
| วงแลน | วงแลน + อินเทอร์เน็ต | ที่ไหนก็ได้ ยกเว้น `file://` | วงแลน + อินเทอร์เน็ต |
| เครื่องนี้ (`localhost`, `lowd`) | ทุกที่ ยกเว้น `file://` | ที่ไหนก็ได้ ยกเว้น `file://` | ทุกที่ |
| ไฟล์ (`file://`) | ไฟล์ในโฟลเดอร์เดียวกันและโฟลเดอร์ย่อย + ทุก server | ที่ไหนก็ได้ | ทุกที่ |

- ตรวจทุก redirect และทุก connection: redirect ไป 127.0.0.1 หรือ `file://` ก็ถูกกัน, connection ที่ค้างใน pool และของใน
  HTTP cache จำไว้ว่ามาจากระดับไหน (หน้าเว็บจากอินเทอร์เน็ตจึงอ่านหน้า router ที่เคยเปิดไว้ใน cache ไม่ได้)
- path ของไฟล์ตรวจหลังแปลงเป็น path เต็มแล้ว (`..`, `%2e%2e`, `%5c` ออกนอกโฟลเดอร์ไม่ได้)
- ผู้ใช้เองพิมพ์ address ไปที่ไหนก็ได้เหมือนเดิม; ที่ถูกกันจะได้ error ที่ขึ้นต้นด้วย `blocked:` และมีบรรทัด `[net] blocked …` ใน log
- ยังไม่ใช่ same-origin policy เต็มรูปแบบ: หน้าเว็บยังอ่านข้อมูลสาธารณะจากเว็บอื่นได้ แต่ส่ง cookie ไปให้เว็บอื่นไม่ได้
  (third-party cookie ถูกปิดอยู่แล้ว) จึงอ่านได้แค่สิ่งที่ใครก็โหลดได้อยู่แล้ว

address ทำงานเหมือนเว็บปกติ:
- path ที่ลงท้ายด้วย `/` จะลองโหลด `index.wasm` ในโฟลเดอร์นั้นก่อน ถ้าไม่มี (หรือไม่ใช่ wasm) จะขอ URL เดิมตรง ๆ
  เว็บทั่วไปจึงได้หน้าแรกปกติของมัน
- HTML → viewer, `text/plain` → browser แสดงเอง, รูป PNG/JPEG/GIF/BMP → เปิดดูได้ตรง ๆ, `file:///` ใช้ตอนพัฒนา

## HTML viewer ทำอะไรได้บ้าง

| ส่วน | รองรับ |
|---|---|
| Charset | UTF-8, windows-874/TIS-620 (เว็บไทยรุ่นเก่า รวมถึง google.com), windows-1252/Latin-1 |
| HTML | tokenizer + tree builder (ปิด tag อัตโนมัติแบบ HTML5 ส่วนใหญ่), character references |
| Layout | block/inline, ตัดบรรทัด (ไทยตัดตามคำด้วย Uniscribe), หัวข้อ, list, `pre`, ตาราง (colspan, ความกว้างอัตโนมัติ), `bgcolor`/`color`/`align`, inline `style` บางส่วน |
| แสดงผลทีละส่วน | HTML ถูกส่งให้ viewer **ระหว่างดาวน์โหลด** (gzip ก็ถอดทีละส่วน) และ parse ไปพร้อมกัน; เจอ `<link rel=stylesheet>` ก็โหลด CSS ทันที; จัดหน้าจากบนลงล่างครั้งละ ~12 ms ต่อเฟรม จอแรกขึ้นทันทีที่จัดเสร็จ ส่วนที่ยังดาวน์โหลดไม่ถึงก็รอ แล้วตามมาเองโดยหน้าต่างไม่ค้าง; ตอน resize จะจัดส่วนที่เห็นก่อน |
| CSS | อ่าน `<style>` และ `<link rel=stylesheet>` เพื่อหาว่าอะไรถูก**ซ่อน** (`display:none`, `visibility:hidden`, ข้อความสำหรับ screen reader); `@media` ตามความกว้างจอ; cascade (specificity, `!important`); คำนวณ CSS ของแต่ละ element ตอนที่การจัดหน้าไปถึง และใช้ Bloom filter ของ ancestor ตัด selector ที่ไม่มีทางตรงออกเร็ว ๆ |
| รูป | PNG, JPEG, GIF, **WebP** (lossy, lossless, alpha, animation เฟรมแรก), **SVG** (ไฟล์ `.svg` และ `<svg>` ที่ฝังในหน้า), BMP; `<picture>`/`srcset`; `data:` URI |
| Reader view | ถ้าเว็บมี `<main>`/`role=main` จะแสดงแค่ส่วนนั้น คลิกป้ายมุมขวาล่างเพื่อสลับไปดูทั้งหน้า |
| Form | text/password/checkbox/radio/select/textarea/submit, ส่งแบบ GET และ POST |
| อื่น ๆ | ลิงก์, `#anchor` (รวมถึง URL ที่มี `#` ตั้งแต่เปิด: เลื่อนไปเมื่อจัดหน้าถึง), `<meta refresh>`, scroll (ล้อเมาส์, คีย์บอร์ด, scrollbar) |

ไม่มี: JavaScript, CSS ส่วนที่เป็นการจัดหน้า (flex, grid, float, position), AVIF
(เว็บที่ต้องใช้ JS อย่างผลค้นหาของ Google จึงใช้ไม่ได้ ส่วน DuckDuckGo Lite, Wikipedia, Hacker News, BBC ใช้ได้)

## เขียนเว็บของตัวเอง

```c
#include "lowweb.h"
static unsigned fb[640 * 360];

LW_EXPORT(lw_start) void lw_start(void) { lw_set_title(LW_STR("Hello")); }

LW_EXPORT(lw_frame) void lw_frame(double ms) {
    for (int i = 0; i < 640 * 360; i++) fb[i] = 0xFF3B2A1E;          // 0xAABBGGRR
    lw_text(fb, 640, 360, 40, 40, LW_STR("สวัสดี Low-web!"), 40, LW_TEXT_BOLD, 0xFFFFFFFF);
    lw_present(fb, 640, 360);
}
```

```bat
clang --target=wasm32 -O2 -mcpu=mvp -mbulk-memory -mnontrapping-fptoint -msign-ext ^
      -mmutable-globals -nostdlib -fno-builtin -Wl,--no-entry -Isdk -o index.wasm hello.c
```

ฟังก์ชันทั้งหมดอยู่ใน [`sdk/lowweb.h`](sdk/lowweb.h) สรุปสั้น ๆ:

| หน้าเว็บ export (browser เรียก) | browser ให้ (หน้าเว็บเรียก) |
|---|---|
| `lw_start`, `lw_resize`, `lw_frame` | `lw_present` ส่งภาพขึ้นจอ |
| `lw_pointer` (เมาส์ → คืน cursor) | `lw_text`, `lw_text_width` (ตัวหนา/เอียง/monospace), `lw_line_breaks` |
| `lw_key`, `lw_char` | `lw_fetch` → ผลกลับมาทาง `lw_on_fetch` / `lw_on_fetch_ex` |
| `lw_alloc` (ให้ browser ขอหน่วยความจำ) | `lw_open_file` → `lw_on_file`, `lw_save_file`, `lw_clipboard_set/get` |
| `lw_on_file`, `lw_on_fetch`, `lw_on_fetch_ex`, `lw_on_fetch_begin/data/end` (เอกสารแบบ stream), `lw_state`/`lw_restore` (ตำแหน่งที่อ่านอยู่, ไม่บังคับ) | `lw_image_decode/read/free`, `lw_navigate`, `lw_navigate_post`, `lw_open_tab`, `lw_mods`, `lw_set_title`, `lw_now`, `lw_scale`, `lw_log` |

## Host จริง

เว็บ Low-web เป็นแค่ไฟล์ static (`.wasm`, `.txt`, รูป) จึง host ได้ 3 แบบ

1. **`lowd` บน VPS / Linux**: `g++ -std=c++17 -O2 -pthread -o lowd server/lowd.cpp` แล้ว
   `./lowd -p 80 www` (หรือพอร์ตอื่น + reverse proxy) ผู้ใช้เปิด `http://ชื่อโดเมน/`
2. **เครื่องตัวเอง**: `bin\lowd.exe sites\www` (ฟังทุก interface, IPv4+IPv6), อนุญาตใน Windows Firewall,
   แล้ว port-forward 8080 ที่ router หรือใช้ tunnel อย่าง Cloudflare Tunnel ถ้าไม่มี public IP
3. **static host ที่มี HTTPS** (GitHub Pages, Cloudflare Pages, Netlify …): อัปโหลดโฟลเดอร์ `www`
   ได้เลย browser รองรับ HTTPS และ redirect เอง ขอแค่ให้มีไฟล์ `index.wasm` ในแต่ละโฟลเดอร์

## การทดสอบ

ทุก push และ pull request บน GitHub ถูก build และทดสอบอัตโนมัติ (`.github/workflows/ci.yml`): `build.cmd` + `tests/run_tests.py`
+ `tests/check_lowd.sh` บน Windows, และ build `lowd` + `check_lowd.sh` บน Ubuntu; ได้ `lowweb.exe` กับ `lowd.exe` เป็น artifact
ให้ดาวน์โหลด (เก็บ 14 วัน) และ warning ของ compiler จะขึ้นเป็น annotation

| คำสั่ง | ทดสอบอะไร |
|---|---|
| `python tests/run_tests.py [ชื่อ…]` | **รันทุกชุดที่ไม่ต้องใช้เน็ต** ในคำสั่งเดียว (ops, viewer, images, stream, cache, cookies, hpack, access, sleep, select) ตามที่ CI รัน: ควรรันก่อน commit |
| `python tests/check_select.py` | เลือก/คัดลอก/วางข้อความ: ทั้งหน้า, ลากเลือก (ข้ามลิงก์), ช่องกรอก (วาง, ตัด, รหัสผ่านคัดลอกไม่ได้), ไฟล์ข้อความ, หน้า error; แถบแท็บ: ส่วนไหนเป็น caption/ปุ่ม/ขอบปรับขนาด, ปุ่มขยายหน้าต่าง |
| `python tests/check_sleep.py` | แท็บหลับแล้วตื่นมาเหมือนเดิมทุก pixel ที่ตำแหน่งเดิม, back และ reload กลับที่เดิม, หน้าที่พิมพ์ไว้และหน้า Low-web ไม่หลับ |
| `python tests/check_access.py` | หน้าเว็บเข้าถึงอะไรได้: ระดับของ IP, หน้าเว็บจากอินเทอร์เน็ต/วงแลนเข้า 127.0.0.1, `localhost`, `[::1]` ไม่ได้, redirect ไป `file://`, ของใน cache, ไฟล์นอกโฟลเดอร์ (`..`, `%2e%2e`, `%5c`) |
| `bash tests/check_lowd.sh [LOWD]` | `lowd`: ส่ง `index.wasm`, redirect โฟลเดอร์, 404, กัน `..`/`%2e%2e`, ปฏิเสธ POST, HEAD |
| `node tests/check_ops.mjs` เทียบกับ `bin\wasmrun build\ops.wasm "int32()" "int64()" "floats()" "control()" "memory_ops()"` | interpreter ให้ผลตรงกับ V8 ทุกบิต (integer, float, control flow, memory, traps); `wasmrun --no-fuse` รันแบบไม่รวมคำสั่ง ต้องได้ผลเท่ากัน |
| `python tests/check_images.py` | decoder ตรงกับ PIL (PNG ทุกชนิด, JPEG baseline/progressive/subsampling/restart/EXIF, GIF, BMP) และ **WebP ตรงกับ libwebp ทุก pixel** |
| `python tests/check_cookies.py` | cookie: กฎทั้งหมดของ jar (`bin\cookietest`: domain, path, Secure, อายุ, SameSite, third-party, prefix, การบันทึกไฟล์) และผ่าน HTTP จริง: cookie จาก redirect ไปถึงหน้าถัดไป, cache ที่ `Vary: Cookie` ไม่ถูกใช้ซ้ำเมื่อ cookie เปลี่ยน |
| `python tests/check_hpack.py` | HPACK ของ HTTP/2 ตรงกับตัวอย่างทุกข้อใน RFC 7541 Appendix C (มี/ไม่มี Huffman, dynamic table เต็มและไล่ออก), static table ตาม Appendix A, encode แล้ว decode กลับได้เหมือนเดิม |
| `bin\fetchtest --parallel [--no-h2] URL…` | ดึงหลาย URL พร้อมกัน: HTTP/2 ใช้ connection เดียว; `--save DIR` เก็บ body ไว้เทียบกับ HTTP/1.1 |
| `python tests/bench_viewer.py` | จับเวลา viewer กับหน้า Wikipedia 3 MB โดยไม่เปิดหน้าต่าง (`bin\viewerbench`) ทั้งแบบมีและไม่มี instruction fusion และตรวจว่าวาดภาพสุดท้ายได้เหมือนกันทุก pixel; `viewerbench --stream N` ส่ง HTML ทีละ N ไบต์เหมือนตอนดาวน์โหลด และพิมพ์ขนาด wasm memory |
| `python tests/check_cache.py` | HTTP cache และ keep-alive กับ server ทดสอบในเครื่อง: `max-age`, `Expires`, `ETag`/`If-None-Match`, `Last-Modified`, `no-store`, redirect ที่ cache ไว้, reload/hard reload, หลาย request บน connection เดียว (รวม chunked), `Connection: close`, connection ที่ server ปิดทิ้งแล้วต้องส่งใหม่ |
| `python tests/check_stream.py` | ถอด gzip/zlib/deflate แบบทีละส่วน (ที่ใช้แสดงหน้าระหว่างดาวน์โหลด) จากชิ้นขนาดสุ่ม ต้องได้ไบต์ตรงกับต้นฉบับ |
| `bin\fetchtest --stream URL…` | รับ body ทีละส่วนจาก network จริง และตรวจว่าตรงกับ body ทั้งก้อน |
| `python tests/check_svg.py` | SVG เทียบกับ Microsoft Edge (headless) แบบวางคู่กัน ภาพอยู่ใน `build/svgtest/*.cmp.png` |
| `python tests/gen_hpack_huffman.py` | สร้าง `browser/hpack_huffman.h` (ตาราง Huffman ของ HPACK จาก RFC 7541) |
| `python tests/gen_webp_tables.py` | สร้าง `browser/webp_tables.h` (ตารางค่าคงที่ของ VP8/VP8L จาก spec) |
| `bin\fetchtest [--exact] [--cache DIR] URL…` | HTTP/HTTPS, redirect, chunked, gzip, ตรวจ certificate, กฎ `index.wasm`; log บอกว่าแต่ละ request มาจาก cache หรือใช้ connection เดิม |
| `bin\lowweb.exe URL --size 1000x680 --log out.log --script "wait 500; click 100 200; key 83 ctrl" --screenshot out.bmp` | ขับ browser อัตโนมัติแล้วถ่ายภาพหน้าจอ (log บอกเวลานับจากเริ่มโหลด เช่น `[page +383 ms] viewer: first screen…`; คำสั่งรอจะรอให้โหลดเสร็จก่อน ยกเว้นสั่ง `async`; `mem` เขียนการใช้หน่วยความจำลง log; `clip TEXT` ใส่ข้อความใน clipboard ของ script (script mode ไม่แตะ clipboard จริง: ทุกการคัดลอกเขียนเป็น `[clipboard] …` ใน log); `hittest X Y` บอกว่าจุดนั้นบนแถบแท็บเป็นอะไร) |

## ข้อจำกัดที่รู้อยู่

- browser รันบน Windows (Win32 + SChannel); `lowd` build และทดสอบบน Linux ใน CI
- หน้าเว็บรันบน UI thread ด้วย interpreter (รวมคำสั่งที่เจอบ่อยเป็นคำสั่งเดียวตอนโหลด + threaded dispatch: เร็วขึ้น ~2 เท่า
  เทียบกับแบบเดิม; ยังไม่มี JIT): งานหนัก ๆ (เช่นเทสีทั้งผืนใน Paint) ใช้เวลา ~0.1 วินาที,
  หน้า Wikipedia ขนาด 3 MB: parse ~0.12 วินาที, จัดหน้าครั้งแรก ~0.29 วินาที (จัดใหม่ตอนรูปมา ~0.08 วินาที),
  จอแรกขึ้นประมาณ 0.4 วินาทีหลังกด Enter ครั้งแรก และ ~0.2 วินาทีเมื่อ CSS อยู่ใน cache แล้ว; หน้า Low-web ที่ค้างเกิน 5 วินาที (viewer: 20 วินาที) จะถูกหยุด
- ยังไม่มี TLS 1.3 (SChannel ของ Windows 10 ยังไม่เปิด TLS 1.3 ให้ฝั่ง client จึงใช้ TLS 1.2)
- กันหน้าเว็บจากอินเทอร์เน็ตไม่ให้เข้าเครื่องนี้/วงแลน/ไฟล์ได้แล้ว (ดู "หน้าเว็บเข้าถึงอะไรได้บ้าง") แต่ยังไม่มี same-origin policy
  เต็มรูปแบบ
