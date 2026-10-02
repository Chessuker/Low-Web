// video.h — video and audio for pages, played by Windows Media Foundation: the codecs that
// come with Windows (H.264, AAC, MP3; HEVC, VP9, AV1 where their extensions are installed),
// decoded on the graphics card when it can. The bytes come through our own network code
// (net::fetch with ranges, kept in a temporary file rather than in memory), so a video
// follows the same rules as anything else the page fetches.
#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cookies.h"
#include "net.h"

namespace video {

enum State { LOADING = 0, PAUSED = 1, PLAYING = 2, ENDED = 3, FAILED = 4 };

struct Info {
    int state = LOADING;
    bool waiting = false;    // playing, but the data isn't there yet
    double time = 0;         // seconds
    double duration = 0;     // seconds; 0 = not known yet
    double buffered = 0;     // how far (seconds) the data goes on from the current time
    int w = 0, h = 0;        // the picture's display size; 0 = no picture (audio, or not known yet)
    std::string error;       // FAILED: why
};

// Starts Media Foundation (once; the UI thread). False, with the reason, if this Windows
// has none (the "N" editions without the Media Feature Pack, some servers).
bool init(std::string &why);

// Where "[video] ..." lines go.
void set_logger(std::function<void(const std::string &)> log);

class Player {
public:
    // Starts downloading `url` (with the page's cookies and access rules) and plays it once
    // play() is called. `notify` gets PostMessage(msg, id, 0) when something changed;
    // the owner then calls poll().
    Player(int id, const std::string &url, const cookies::Context &who, const net::Access &access, HWND notify, UINT msg);
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    void poll();  // on the UI thread: hands the download to Media Foundation once it has begun
    void play();
    void pause();
    void seek(double seconds);
    void set_volume(double volume, bool muted);
    Info info();
    // The current picture, fitted into w x h (keeping its shape). If it is new since the last
    // call (or `force`), writes fw x fh BGRA pixels to `out` and returns true.
    bool picture(int w, int h, bool force, std::vector<uint32_t> &out, int &fw, int &fh);

    struct Impl;

private:
    std::unique_ptr<Impl> p_;
};

}  // namespace video
