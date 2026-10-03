// video.cpp — see video.h.
//
// Media Foundation's media engine (the one behind <video> in old Edge) does the playing:
// it finds the container and codecs, decodes on the GPU, plays the sound and keeps it in
// step with the picture. We give it:
//   - the bytes, through ByteStream (an IMFByteStream) over a Loader, which downloads with
//     our net::fetch into a temporary file: ranges for seeking, and a pause when far enough
//     ahead of what was read, so a long video isn't downloaded whole for a minute of watching;
//   - a D3D11 device; each new picture is scaled on the GPU to the size it is shown at
//     (TransferVideoFrame), copied back and drawn into the page's frame by main.cpp.
// mfplat.dll is loaded when first needed, so the browser still starts without it.
#include <initguid.h>  // (defines the GUIDs the headers below declare)

#include "video.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfmediaengine.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace video {
namespace {

std::function<void(const std::string &)> g_logger;
void log_line(const std::string &s) {
    if (g_logger) g_logger(s);
}

template <class T>
struct Com {  // holds one reference
    T *p = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T **put() {
        reset();
        return &p;
    }
    void **put_void() { return (void **)put(); }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ---- Media Foundation, loaded on demand -------------------------------------------------

HRESULT(WINAPI *pMFStartup)(ULONG, DWORD);
HRESULT(WINAPI *pMFCreateAttributes)(IMFAttributes **, UINT32);
HRESULT(WINAPI *pMFCreateDXGIDeviceManager)(UINT *, IMFDXGIDeviceManager **);
HRESULT(WINAPI *pMFCreateAsyncResult)(IUnknown *, IMFAsyncCallback *, IUnknown *, IMFAsyncResult **);
HRESULT(WINAPI *pMFInvokeCallback)(IMFAsyncResult *);

bool g_tried = false, g_ok = false;
std::string g_why;
Com<ID3D11Device> g_dev;
Com<ID3D11DeviceContext> g_ctx;
Com<IMFDXGIDeviceManager> g_mgr;
Com<IMFMediaEngineClassFactory> g_factory;

std::string hr_text(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr);
    return b;
}

bool start_mf(std::string &why) {
    HMODULE mf = LoadLibraryW(L"mfplat.dll");
    if (!mf) { why = "Media Foundation is not installed on this Windows (an \"N\" edition needs the Media Feature Pack)"; return false; }
    pMFStartup = (decltype(pMFStartup))(void (*)())GetProcAddress(mf, "MFStartup");
    pMFCreateAttributes = (decltype(pMFCreateAttributes))(void (*)())GetProcAddress(mf, "MFCreateAttributes");
    pMFCreateDXGIDeviceManager = (decltype(pMFCreateDXGIDeviceManager))(void (*)())GetProcAddress(mf, "MFCreateDXGIDeviceManager");
    pMFCreateAsyncResult = (decltype(pMFCreateAsyncResult))(void (*)())GetProcAddress(mf, "MFCreateAsyncResult");
    pMFInvokeCallback = (decltype(pMFInvokeCallback))(void (*)())GetProcAddress(mf, "MFInvokeCallback");
    if (!pMFStartup || !pMFCreateAttributes || !pMFCreateDXGIDeviceManager || !pMFCreateAsyncResult || !pMFInvokeCallback) {
        why = "this Media Foundation is too old";
        return false;
    }
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // (S_FALSE: already)
    (void)hr;
    if (FAILED(hr = pMFStartup(MF_VERSION, MFSTARTUP_FULL))) { why = "MFStartup failed " + hr_text(hr); return false; }
    // a D3D11 device for decoding and scaling (the graphics card's, else Windows' software one)
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    const char *driver = "hardware";
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 5, D3D11_SDK_VERSION, g_dev.put(), nullptr, g_ctx.put());
    if (FAILED(hr)) {
        driver = "WARP";
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 5, D3D11_SDK_VERSION, g_dev.put(), nullptr, g_ctx.put());
    }
    if (FAILED(hr)) { why = "no Direct3D 11 device " + hr_text(hr); return false; }
    Com<ID3D10Multithread> mt;  // Media Foundation uses the device from its own threads too
    if (SUCCEEDED(g_dev->QueryInterface(IID_ID3D10Multithread, mt.put_void()))) mt->SetMultithreadProtected(TRUE);
    UINT token = 0;
    if (FAILED(hr = pMFCreateDXGIDeviceManager(&token, g_mgr.put())) || FAILED(hr = g_mgr->ResetDevice(g_dev.p, token))) {
        why = "no DXGI device manager " + hr_text(hr);
        return false;
    }
    if (FAILED(hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IMFMediaEngineClassFactory,
                                     g_factory.put_void()))) {
        why = "no media engine " + hr_text(hr);
        return false;
    }
    log_line(std::string("[video] Media Foundation ready (Direct3D 11, ") + driver + ")");
    return true;
}

// ---- the download -------------------------------------------------------------------------

const int64_t kAhead = 32ll << 20;   // download at most this far past what was read, then rest
const int64_t kResume = 8ll << 20;   // and go on when the reader comes this close to the end of it
const double kNearSeconds = 1.0;     // a read this little past where a download is (at its speed) waits for it

struct Loader : std::enable_shared_from_this<Loader> {
    std::string url;
    cookies::Context who;
    net::Access access;
    HWND hwnd = nullptr;
    UINT msg = 0;
    int id = 0;

    std::mutex m;  // everything below
    std::condition_variable cv;
    HANDLE file = INVALID_HANDLE_VALUE;  // what has arrived, at its place in the whole
    bool head = false, closed = false;
    bool ranges = true;     // the server answers range requests (else: one download, start to end)
    int64_t size = -1;      // of the whole; -1 = unknown
    std::string type, error;
    std::vector<std::pair<int64_t, int64_t>> have;  // what is in the file: [from, to), sorted, apart
    int64_t read_to = 0;    // the furthest byte read so far
    int failures = 0;       // downloads in a row that ended in an error

    struct Job : net::Stream {
        Loader *L = nullptr;
        int64_t from = 0, pos = 0;  // asked from; where the next byte goes
        DWORD t0 = GetTickCount();  // when it began, for its speed
        bool begin(const net::Response &h) override {
            std::lock_guard<std::mutex> lk(L->m);
            if (h.status != 200 && h.status != 206) return false;
            if (from > 0 && h.range_start != from) L->ranges = false;  // it sent the whole thing again
            pos = h.range_start;
            if (!L->head) {
                L->head = true;
                L->type = h.content_type;
            }
            if (L->size < 0) L->size = h.total_size;
            L->cv.notify_all();
            PostMessageW(L->hwnd, L->msg, (WPARAM)L->id, 0);
            return true;
        }
        void data(const uint8_t *d, size_t n) override {
            OVERLAPPED o{};
            o.Offset = (DWORD)pos;
            o.OffsetHigh = (DWORD)(pos >> 32);
            DWORD wrote = 0;
            if (!WriteFile(L->file, d, (DWORD)n, &wrote, &o) || wrote != n) {
                stop = true;
                return;
            }
            std::lock_guard<std::mutex> lk(L->m);
            L->add(pos, pos + (int64_t)n);
            pos += (int64_t)n;
            L->failures = 0;
            if (L->ranges && pos > L->read_to + kAhead) stop = true;  // far enough ahead: rest
            L->cv.notify_all();
        }
    };
    std::shared_ptr<Job> job;  // the download going on, if any

    bool open() {
        wchar_t dir[MAX_PATH], name[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"lwv", 0, name)) return false;
        file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        return file != INVALID_HANDLE_VALUE;
    }
    ~Loader() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    void add(int64_t a, int64_t b) {
        auto it = std::lower_bound(have.begin(), have.end(), std::make_pair(a, a));
        if (it != have.begin() && std::prev(it)->second >= a) --it;
        while (it != have.end() && it->first <= b) {  // merge what it touches
            a = std::min(a, it->first);
            b = std::max(b, it->second);
            it = have.erase(it);
        }
        have.insert(it, {a, b});
    }
    int64_t there_from(int64_t pos) const {  // bytes in the file from pos on, without a gap
        auto it = std::upper_bound(have.begin(), have.end(), std::make_pair(pos, INT64_MAX));
        if (it == have.begin()) return 0;
        --it;
        return it->first <= pos && pos < it->second ? it->second - pos : 0;
    }

    void start(int64_t from) {  // (m held)
        log_line("[video] " + std::to_string(id) + " download from byte " + std::to_string(from));
        if (job) job->stop = true;
        auto j = std::make_shared<Job>();
        j->L = this;
        j->from = j->pos = from;
        j->range_from = from;
        job = j;
        std::thread([self = shared_from_this(), j] { self->run(j); }).detach();
    }
    void run(std::shared_ptr<Job> j) {
        net::Response r = net::fetch(url, net::Mode::Exact, (size_t)1 << 60, nullptr, j.get(), net::CacheMode::Normal, &who, access);
        std::lock_guard<std::mutex> lk(m);
        if (job == j) job = nullptr;
        if (r.status == 0 && r.error != "stopped") {
            log_line("[video] download from byte " + std::to_string(j->from) + " failed: " + r.error);
            if (!head || ++failures >= 3) error = r.error;
        } else if (!head && r.status && r.status != 200 && r.status != 206) {
            error = "the server answered " + std::to_string(r.status);
        }
        cv.notify_all();
        PostMessageW(hwnd, msg, (WPARAM)id, 0);
    }

    // Up to n bytes at pos: waits until they are there. 0 at the end, -1 if closed or failed.
    int read(int64_t pos, uint8_t *buf, int n) {
        std::unique_lock<std::mutex> lk(m);
        for (;;) {
            if (closed) return -1;
            if (head && size >= 0 && pos >= size) return 0;
            int64_t avail = there_from(pos);
            if (avail > 0) {
                int k = (int)std::min<int64_t>(avail, n);
                read_to = std::max(read_to, pos + k);
                int64_t end = pos + avail;  // keep the data coming before the reader gets there
                if (!job && ranges && (size < 0 || end < size) && avail < kResume) start(end);
                lk.unlock();
                OVERLAPPED o{};
                o.Offset = (DWORD)pos;
                o.OffsetHigh = (DWORD)(pos >> 32);
                DWORD got = 0;
                if (!ReadFile(file, buf, (DWORD)k, &got, &o)) return -1;
                return (int)got;
            }
            if (!error.empty()) return -1;
            read_to = std::max(read_to, pos);
            bool coming = job && job->pos <= pos && (pos - job->pos < wait_room(*job) || !ranges);
            if (!coming && (ranges || !job)) start(ranges ? pos : 0);
            cv.wait_for(lk, std::chrono::milliseconds(500));
        }
    }
    // How far ahead of a download a read may be and still wait for it: what it brings in a
    // second or so (a new request costs a round trip, often more than that).
    static int64_t wait_room(const Job &j) {
        double s = (GetTickCount() - j.t0) / 1000.0;
        int64_t got = j.pos - j.from;
        return std::max<int64_t>(256 << 10, s > 0.5 ? (int64_t)(got / s * kNearSeconds) : 0);
    }
    // Seconds of media there are, without a gap, from time t on (assuming an even bit rate).
    double seconds_from(double t, double duration) {
        std::lock_guard<std::mutex> lk(m);
        if (size <= 0 || duration <= 0) return 0;
        int64_t at = (int64_t)(t / duration * (double)size);
        int64_t there = there_from(std::min(at, size - 1));
        return std::max(0.0, std::min(duration - t, there / (double)size * duration));
    }
    void wait_head() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return head || closed || !error.empty(); });
    }
    void close() {
        std::lock_guard<std::mutex> lk(m);
        closed = true;
        if (job) job->stop = true;
        cv.notify_all();
    }
};

// ---- the bytes, for Media Foundation ------------------------------------------------------

// Reads asked for with BeginRead are done one after another on a thread of their own (they
// may wait for the network).
struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    bool quit = false;
};

struct ReadOp : IUnknown {  // the state of one BeginRead, kept in its IMFAsyncResult
    LONG refs = 1;
    QWORD at = 0;
    ULONG got = 0;
    STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
        if (iid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (!n) delete this;
        return (ULONG)n;
    }
    virtual ~ReadOp() = default;
};

// All of cb bytes (fewer only at the end): Media Foundation's file parsers take a short read
// for the end of the file (and then say it is damaged).
HRESULT fill(Loader &L, QWORD at, BYTE *pb, ULONG cb, ULONG &got) {
    got = 0;
    while (got < cb) {
        int k = L.read((int64_t)(at + got), pb + got, (int)std::min<ULONG>(cb - got, 1u << 24));
        if (k < 0) return E_FAIL;
        if (k == 0) break;  // the end
        got += (ULONG)k;
    }
    return S_OK;
}

class ByteStream : public IMFByteStream {
public:
    explicit ByteStream(std::shared_ptr<Loader> L) : L_(std::move(L)), w_(std::make_shared<Worker>()) {
        std::thread([w = w_] {
            for (;;) {
                std::function<void()> f;
                {
                    std::unique_lock<std::mutex> lk(w->m);
                    w->cv.wait(lk, [&] { return w->quit || !w->q.empty(); });
                    if (w->q.empty()) return;
                    f = std::move(w->q.front());
                    w->q.pop_front();
                }
                f();
            }
        }).detach();
    }
    virtual ~ByteStream() {
        std::lock_guard<std::mutex> lk(w_->m);
        w_->quit = true;
        w_->cv.notify_all();
    }

    STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
        if (iid == __uuidof(IUnknown) || iid == IID_IMFByteStream) {
            *out = static_cast<IMFByteStream *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return (ULONG)n;
    }

    STDMETHODIMP GetCapabilities(DWORD *c) override {
        L_->wait_head();
        std::lock_guard<std::mutex> lk(L_->m);
        *c = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE | (L_->ranges ? 0 : MFBYTESTREAM_HAS_SLOW_SEEK);
        return S_OK;
    }
    STDMETHODIMP GetLength(QWORD *len) override {
        L_->wait_head();
        std::lock_guard<std::mutex> lk(L_->m);
        *len = L_->size >= 0 ? (QWORD)L_->size : (QWORD)-1;
        return S_OK;
    }
    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPosition(QWORD *pos) override {
        std::lock_guard<std::mutex> lk(m_);
        *pos = pos_;
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD pos) override {
        std::lock_guard<std::mutex> lk(m_);
        pos_ = pos;
        return S_OK;
    }
    STDMETHODIMP IsEndOfStream(BOOL *end) override {
        QWORD len = 0;
        GetLength(&len);
        std::lock_guard<std::mutex> lk(m_);
        *end = len != (QWORD)-1 && pos_ >= len;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE *pb, ULONG cb, ULONG *read) override {
        QWORD at;
        {
            std::lock_guard<std::mutex> lk(m_);
            at = pos_;
        }
        ULONG got = 0;
        HRESULT hr = fill(*L_, at, pb, cb, got);
        std::lock_guard<std::mutex> lk(m_);
        pos_ = at + got;
        *read = got;
        return hr;
    }
    STDMETHODIMP BeginRead(BYTE *pb, ULONG cb, IMFAsyncCallback *cb_obj, IUnknown *state) override {
        auto *op = new ReadOp;
        {
            std::lock_guard<std::mutex> lk(m_);
            op->at = pos_;
        }
        IMFAsyncResult *res = nullptr;
        HRESULT hr = pMFCreateAsyncResult(op, cb_obj, state, &res);
        op->Release();
        if (FAILED(hr)) return hr;
        std::shared_ptr<Loader> L = L_;
        std::lock_guard<std::mutex> lk(w_->m);
        w_->q.push_back([L, op, pb, cb, res] {  // (res holds op)
            ULONG got = 0;
            HRESULT r = fill(*L, op->at, pb, cb, got);
            op->got = got;
            res->SetStatus(r);
            pMFInvokeCallback(res);
            res->Release();
        });
        w_->cv.notify_all();
        return S_OK;
    }
    STDMETHODIMP EndRead(IMFAsyncResult *res, ULONG *read) override {
        Com<IUnknown> obj;
        HRESULT hr = res->GetObject(obj.put());
        if (FAILED(hr)) return hr;
        auto *op = static_cast<ReadOp *>(obj.p);
        std::lock_guard<std::mutex> lk(m_);
        pos_ = op->at + op->got;
        *read = op->got;
        return res->GetStatus();
    }
    STDMETHODIMP Write(const BYTE *, ULONG, ULONG *) override { return E_NOTIMPL; }
    STDMETHODIMP BeginWrite(const BYTE *, ULONG, IMFAsyncCallback *, IUnknown *) override { return E_NOTIMPL; }
    STDMETHODIMP EndWrite(IMFAsyncResult *, ULONG *) override { return E_NOTIMPL; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG off, DWORD, QWORD *cur) override {
        std::lock_guard<std::mutex> lk(m_);
        LONGLONG to = origin == msoCurrent ? (LONGLONG)pos_ + off : off;
        pos_ = (QWORD)std::max<LONGLONG>(0, to);
        if (cur) *cur = pos_;
        return S_OK;
    }
    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override {
        L_->close();
        return S_OK;
    }

private:
    LONG refs_ = 1;
    std::shared_ptr<Loader> L_;
    std::shared_ptr<Worker> w_;
    std::mutex m_;
    QWORD pos_ = 0;
};

// ---- what the media engine tells us -----------------------------------------------------

struct Events {
    std::mutex m;
    int error = 0;  // MF_MEDIA_ENGINE_ERR_*
    HRESULT error_hr = S_OK;
    HWND hwnd = nullptr;
    UINT msg = 0;
    int id = 0;
};

class Notify : public IMFMediaEngineNotify {
public:
    explicit Notify(std::shared_ptr<Events> e) : e_(std::move(e)) {}
    virtual ~Notify() = default;
    STDMETHODIMP QueryInterface(REFIID iid, void **out) override {
        if (iid == __uuidof(IUnknown) || iid == IID_IMFMediaEngineNotify) {
            *out = static_cast<IMFMediaEngineNotify *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return (ULONG)n;
    }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) SetEvent((HANDLE)p1);  // (it waits for this)
        if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) {
            std::lock_guard<std::mutex> lk(e_->m);
            e_->error = (int)p1;
            e_->error_hr = (HRESULT)p2;
        }
        PostMessageW(e_->hwnd, e_->msg, (WPARAM)e_->id, 0);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    std::shared_ptr<Events> e_;
};

// A name for the media engine to tell the kind of file by (it goes by the extension). Every
// MP4 (.m4a audio, .m4v) is called .mp4: the same reader takes them all, and some Windows
// (Server) have it registered only for .mp4.
std::string name_for(const std::string &type, const std::string &url) {
    std::string t;
    for (char c : type.substr(0, type.find(';'))) t += (char)tolower((unsigned char)c);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    static const char *const known[][2] = {
        {"video/mp4", "mp4"}, {"audio/mp4", "mp4"}, {"audio/x-m4a", "mp4"}, {"video/x-m4v", "mp4"}, {"video/quicktime", "mov"}, {"video/webm", "webm"},
        {"audio/webm", "webm"}, {"audio/mpeg", "mp3"}, {"audio/mp3", "mp3"}, {"audio/wav", "wav"}, {"audio/x-wav", "wav"},
        {"audio/wave", "wav"}, {"audio/aac", "aac"}, {"video/x-msvideo", "avi"}, {"video/3gpp", "3gp"}, {"video/mp2t", "ts"},
        {"audio/ogg", "ogg"}, {"video/ogg", "ogv"}, {"audio/flac", "flac"}, {"video/x-ms-wmv", "wmv"}, {"audio/x-ms-wma", "wma"}};
    for (auto &k : known)
        if (t == k[0]) return std::string("media.") + k[1];
    std::string path = url.substr(0, url.find_first_of("?#"));  // else the address's own extension
    size_t dot = path.rfind('.'), slash = path.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && path.size() - dot <= 5) {
        std::string ext = path.substr(dot);
        for (char &c : ext) c = (char)tolower((unsigned char)c);
        return ext == ".m4a" || ext == ".m4v" ? "media.mp4" : "media" + ext;
    }
    return "media.mp4";
}

}  // namespace

void set_logger(std::function<void(const std::string &)> log) { g_logger = std::move(log); }

bool init(std::string &why) {
    if (!g_tried) {
        g_tried = true;
        g_ok = start_mf(g_why);
        if (!g_ok) log_line("[video] not available: " + g_why);
    }
    why = g_why;
    return g_ok;
}

// ---- the player -------------------------------------------------------------------------------

struct Player::Impl {
    int id = 0;
    std::string url, fail;
    std::shared_ptr<Loader> loader;
    std::shared_ptr<Events> events;
    Com<IMFMediaEngine> engine;
    Com<IMFMediaEngineEx> ex;
    bool source_set = false, want_play = false;
    double want_seek = -1;
    Com<ID3D11Texture2D> tex, staging;
    int tw = 0, th = 0;
    int asked_w = 0, asked_h = 0;  // the last picture()'s box
    bool pending = false;          // a picture is on its way to `staging`
};

Player::Player(int id, const std::string &url, const cookies::Context &who, const net::Access &access, HWND notify, UINT msg)
    : p_(std::make_unique<Impl>()) {
    Impl &p = *p_;
    p.id = id;
    p.url = url;
    std::string why;
    if (!init(why)) { p.fail = why; return; }
    p.events = std::make_shared<Events>();
    p.events->hwnd = notify;
    p.events->msg = msg;
    p.events->id = id;
    Com<IMFAttributes> attr;
    HRESULT hr = pMFCreateAttributes(attr.put(), 3);
    auto *cb = new Notify(p.events);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, cb);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, g_mgr.p);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr)) hr = g_factory->CreateInstance(0, attr.p, p.engine.put());
    cb->Release();
    if (SUCCEEDED(hr)) hr = p.engine->QueryInterface(IID_IMFMediaEngineEx, p.ex.put_void());
    if (FAILED(hr)) {
        p.fail = "could not start the media engine " + hr_text(hr);
        return;
    }
    p.loader = std::make_shared<Loader>();
    Loader &L = *p.loader;
    L.url = url;
    L.who = who;
    L.access = access;
    L.hwnd = notify;
    L.msg = msg;
    L.id = id;
    if (!L.open()) {
        p.fail = "could not make a temporary file";
        return;
    }
    std::lock_guard<std::mutex> lk(L.m);
    L.start(0);
}

Player::~Player() {
    Impl &p = *p_;
    if (p.loader) p.loader->close();  // (lets reads waiting for the network return)
    if (p.engine) p.engine->Shutdown();
}

void Player::poll() {
    Impl &p = *p_;
    if (p.source_set || !p.fail.empty() || !p.engine) return;
    std::string type;
    {
        std::lock_guard<std::mutex> lk(p.loader->m);
        if (!p.loader->error.empty()) {
            p.fail = p.loader->error;
            return;
        }
        if (!p.loader->head) return;
        type = p.loader->type;
    }
    // the answer has begun: now the engine can look at the bytes
    auto *bs = new ByteStream(p.loader);
    std::wstring name;
    for (char c : name_for(type, p.url)) name += (wchar_t)(unsigned char)c;
    BSTR b = SysAllocString(name.c_str());
    HRESULT hr = p.ex->SetSourceFromByteStream(bs, b);
    SysFreeString(b);
    bs->Release();
    if (FAILED(hr)) {
        p.fail = "the media engine refused it " + hr_text(hr);
        return;
    }
    p.source_set = true;
    if (p.want_seek >= 0) p.engine->SetCurrentTime(p.want_seek);
    if (p.want_play) p.engine->Play();
}

void Player::play() {
    Impl &p = *p_;
    p.want_play = true;
    if (p.source_set) p.engine->Play();
}

void Player::pause() {
    Impl &p = *p_;
    p.want_play = false;
    if (p.source_set) p.engine->Pause();
}

void Player::seek(double s) {
    Impl &p = *p_;
    if (!(s >= 0)) s = 0;
    p.want_seek = s;
    if (p.source_set) p.engine->SetCurrentTime(s);
}

void Player::set_volume(double v, bool muted) {
    Impl &p = *p_;
    if (!p.engine) return;
    p.engine->SetVolume(std::min(1.0, std::max(0.0, v)));
    p.engine->SetMuted(muted);
}

Info Player::info() {
    Impl &p = *p_;
    Info i;
    if (p.fail.empty() && p.events) {
        std::lock_guard<std::mutex> lk(p.events->m);
        if (p.events->error) {
            std::string loader_error;
            {
                std::lock_guard<std::mutex> lk2(p.loader->m);
                loader_error = p.loader->error;
            }
            HRESULT hr = p.events->error_hr;
            if (hr == MF_E_NO_AUDIO_PLAYBACK_DEVICE || hr == MF_E_AUDIO_SERVICE_NOT_RUNNING) {
                p.fail = "this computer has no sound device, and Windows plays videos only with one";
            } else switch (p.events->error) {
            case MF_MEDIA_ENGINE_ERR_NETWORK: p.fail = loader_error.empty() ? "the download failed" : loader_error; break;
            case MF_MEDIA_ENGINE_ERR_DECODE: p.fail = "the file is damaged or can't be decoded"; break;
            case MF_MEDIA_ENGINE_ERR_SRC_NOT_SUPPORTED:
                p.fail = !loader_error.empty() ? loader_error : "Windows can't play this kind of file (its codec isn't installed)";
                break;
            case MF_MEDIA_ENGINE_ERR_ENCRYPTED: p.fail = "the video is encrypted (DRM)"; break;
            default: p.fail = "playback failed " + hr_text(p.events->error_hr);
            }
            log_line("[video] " + std::to_string(p.id) + " failed: " + p.fail + " (" + hr_text(p.events->error_hr) + ")");
        }
    }
    if (!p.fail.empty()) {
        i.state = FAILED;
        i.error = p.fail;
        return i;
    }
    if (!p.source_set) return i;  // LOADING
    IMFMediaEngine *e = p.engine.p;
    USHORT ready = e->GetReadyState();
    double t = e->GetCurrentTime(), d = e->GetDuration();
    i.time = std::isfinite(t) ? t : 0;
    i.duration = std::isfinite(d) && d > 0 ? d : 0;
    if (ready < MF_MEDIA_ENGINE_READY_HAVE_METADATA) i.state = LOADING;
    else if (e->IsEnded()) i.state = ENDED;
    else if (e->IsPaused()) i.state = PAUSED;
    else {
        i.state = PLAYING;
        i.waiting = ready < MF_MEDIA_ENGINE_READY_HAVE_FUTURE_DATA || e->IsSeeking();
    }
    if (ready >= MF_MEDIA_ENGINE_READY_HAVE_METADATA && e->HasVideo()) {
        DWORD w = 0, h = 0, ax = 0, ay = 0;
        if (SUCCEEDED(e->GetNativeVideoSize(&w, &h)) && w && h) {
            i.w = (int)w;
            i.h = (int)h;
            if (SUCCEEDED(e->GetVideoAspectRatio(&ax, &ay)) && ax && ay) i.w = (int)std::lround((double)h * ax / ay);
        }
    }
    // (the engine thinks a byte stream that isn't a network one is all there: ask the download)
    i.buffered = p.loader->seconds_from(i.time, i.duration);
    return i;
}

// The pictures go GPU -> CPU without waiting for the GPU: a new one is copied to a staging
// texture now and read on the next call (the next timer tick, 15 ms on, when the copy is long
// done). Waiting for it at once took ~2 ms of the UI thread per picture.
bool Player::picture(int w, int h, bool force, std::vector<uint32_t> &out, int &fw, int &fh) {
    Impl &p = *p_;
    if (!p.source_set || w <= 0 || h <= 0) return false;
    bool got = false;
    if (p.pending) {  // the copy made last time
        D3D11_MAPPED_SUBRESOURCE mr{};
        HRESULT hr = g_ctx->Map(p.staging.p, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mr);
        if (SUCCEEDED(hr)) {
            fw = p.tw;
            fh = p.th;
            out.resize((size_t)fw * fh);
            for (int y = 0; y < fh; y++) {
                const uint32_t *row = (const uint32_t *)((const uint8_t *)mr.pData + (size_t)y * mr.RowPitch);
                uint32_t *o = out.data() + (size_t)y * fw;
                for (int x = 0; x < fw; x++) o[x] = row[x] | 0xFF000000u;
            }
            g_ctx->Unmap(p.staging.p, 0);
            p.pending = false;
            got = true;
        } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
            p.pending = false;
        }
    }
    LONGLONG pts = 0;
    bool fresh = p.engine->OnVideoStreamTick(&pts) == S_OK;
    if (!fresh && !force && w == p.asked_w && h == p.asked_h) return got;  // (most calls: nothing new)
    p.asked_w = w;
    p.asked_h = h;
    if (!p.engine->HasVideo()) return got;
    DWORD nw = 0, nh = 0, ax = 0, ay = 0;
    if (FAILED(p.engine->GetNativeVideoSize(&nw, &nh)) || !nw || !nh) return got;
    double vw = nw, vh = nh;
    if (SUCCEEDED(p.engine->GetVideoAspectRatio(&ax, &ay)) && ax && ay) vw = vh * ax / ay;
    int tw = w, th = (int)std::lround(w * vh / vw);  // fitted into w x h, keeping its shape
    if (th > h) {
        th = h;
        tw = (int)std::lround(h * vw / vh);
    }
    tw = std::max(1, std::min(tw, 8192));
    th = std::max(1, std::min(th, 8192));
    if (tw != p.tw || th != p.th) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = (UINT)tw;
        d.Height = (UINT)th;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        p.tw = p.th = 0;
        p.pending = false;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, p.tex.put()))) return got;
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, p.staging.put()))) return got;
        p.tw = tw;
        p.th = th;
    } else if (p.pending) {
        return got;  // the last copy isn't done yet: this picture is skipped
    }
    RECT dst{0, 0, tw, th};
    MFARGB black{0, 0, 0, 255};
    if (FAILED(p.engine->TransferVideoFrame(p.tex.p, nullptr, &dst, &black))) return got;
    g_ctx->CopyResource(p.staging.p, p.tex.p);
    p.pending = true;
    return got;
}

}  // namespace video
