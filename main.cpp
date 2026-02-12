#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cwchar>
#include <iostream>
#include <locale.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <ncursesw/ncurses.h>

extern "C" {
#include <libavformat/avformat.h>
}

namespace {

constexpr int kSpecWidth = 20;
constexpr int kSpecHeight = 10;
constexpr gint64 kRefreshNs = 100'000'000;  // 100 ms

struct TrackInfo {
    std::string path;
    std::string display_name;
    double duration_sec = 0.0;
};

class FfmpegProbe {
public:
    static std::optional<TrackInfo> Probe(const std::string& path) {
        AVFormatContext* ctx = nullptr;
        if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) {
            return std::nullopt;
        }

        TrackInfo out;
        out.path = path;

        if (avformat_find_stream_info(ctx, nullptr) >= 0 && ctx->duration > 0) {
            out.duration_sec = static_cast<double>(ctx->duration) / AV_TIME_BASE;
        }

        std::string name = path;
        const auto slash = name.find_last_of('/');
        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        out.display_name = name;

        avformat_close_input(&ctx);
        return out;
    }
};

class Player {
public:
    explicit Player(std::vector<TrackInfo> tracks) : tracks_(std::move(tracks)) {
        playbin_ = gst_element_factory_make("playbin", "playbin");
        spectrum_ = gst_element_factory_make("spectrum", "spectrum");
        if (!playbin_ || !spectrum_) {
            throw std::runtime_error("Failed to create GStreamer elements");
        }

        g_object_set(G_OBJECT(spectrum_),
                     "bands", kSpecWidth,
                     "interval", kRefreshNs,
                     "threshold", -80,
                     "message", TRUE,
                     nullptr);

        g_object_set(G_OBJECT(playbin_), "audio-filter", spectrum_, nullptr);

        bus_ = gst_element_get_bus(playbin_);
        if (!bus_) {
            throw std::runtime_error("Failed to get GStreamer bus");
        }

        load_track(0);
    }

    ~Player() {
        if (playbin_) {
            gst_element_set_state(playbin_, GST_STATE_NULL);
        }
        if (bus_) {
            gst_object_unref(bus_);
        }
        if (playbin_) {
            gst_object_unref(playbin_);
        }
        if (spectrum_) {
            gst_object_unref(spectrum_);
        }
    }

    void toggle_play_pause() {
        if (state_ == State::Stopped || state_ == State::Paused) {
            gst_element_set_state(playbin_, GST_STATE_PLAYING);
            state_ = State::Playing;
            return;
        }
        if (state_ == State::Playing) {
            gst_element_set_state(playbin_, GST_STATE_PAUSED);
            state_ = State::Paused;
        }
    }

    void stop() {
        gst_element_set_state(playbin_, GST_STATE_READY);
        state_ = State::Stopped;
        bars_.fill(0.0f);
    }

    void next_track() {
        if (tracks_.empty()) {
            return;
        }
        load_track((index_ + 1) % tracks_.size());
        gst_element_set_state(playbin_, GST_STATE_PLAYING);
        state_ = State::Playing;
    }

    void prev_track() {
        if (tracks_.empty()) {
            return;
        }
        const size_t next = (index_ == 0) ? tracks_.size() - 1 : index_ - 1;
        load_track(next);
        gst_element_set_state(playbin_, GST_STATE_PLAYING);
        state_ = State::Playing;
    }

    void update() {
        while (true) {
            GstMessage* msg = gst_bus_pop_filtered(
                bus_, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS |
                                                  GST_MESSAGE_ELEMENT | GST_MESSAGE_STATE_CHANGED));
            if (!msg) {
                break;
            }

            switch (GST_MESSAGE_TYPE(msg)) {
                case GST_MESSAGE_EOS:
                    next_track();
                    break;
                case GST_MESSAGE_ELEMENT:
                    parse_spectrum_message(msg);
                    break;
                case GST_MESSAGE_ERROR: {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    if (err) {
                        last_error_ = err->message ? err->message : "Unknown error";
                        g_error_free(err);
                    }
                    if (dbg) {
                        g_free(dbg);
                    }
                    break;
                }
                case GST_MESSAGE_STATE_CHANGED: {
                    if (GST_MESSAGE_SRC(msg) == GST_OBJECT(playbin_)) {
                        GstState old_state, new_state, pending;
                        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                        (void)old_state;
                        (void)pending;
                        if (new_state == GST_STATE_PLAYING) {
                            state_ = State::Playing;
                        } else if (new_state == GST_STATE_PAUSED) {
                            state_ = State::Paused;
                        } else if (new_state <= GST_STATE_READY) {
                            state_ = State::Stopped;
                        }
                    }
                    break;
                }
                default:
                    break;
            }
            gst_message_unref(msg);
        }
    }

    const std::array<float, kSpecWidth>& bars() const { return bars_; }

    const TrackInfo& current_track() const { return tracks_.at(index_); }

    size_t index() const { return index_; }

    size_t count() const { return tracks_.size(); }

    std::string state_text() const {
        switch (state_) {
            case State::Playing:
                return "Playing";
            case State::Paused:
                return "Paused";
            case State::Stopped:
                return "Stopped";
        }
        return "Unknown";
    }

    const std::string& last_error() const { return last_error_; }

    bool is_playing() const { return state_ == State::Playing; }

private:
    enum class State { Playing, Paused, Stopped };

    void load_track(size_t new_index) {
        index_ = new_index;
        gchar* uri = gst_filename_to_uri(tracks_[index_].path.c_str(), nullptr);
        if (!uri) {
            last_error_ = "Cannot build URI for track";
            return;
        }
        gst_element_set_state(playbin_, GST_STATE_READY);
        g_object_set(G_OBJECT(playbin_), "uri", uri, nullptr);
        g_free(uri);
        bars_.fill(0.0f);
    }

    void parse_spectrum_message(GstMessage* msg) {
        const GstStructure* s = gst_message_get_structure(msg);
        if (!s || !gst_structure_has_name(s, "spectrum")) {
            return;
        }

        const GValue* magnitudes = gst_structure_get_value(s, "magnitude");
        if (!magnitudes || !GST_VALUE_HOLDS_LIST(magnitudes)) {
            return;
        }

        const guint n = gst_value_list_get_size(magnitudes);
        const guint limit = std::min(static_cast<guint>(kSpecWidth), n);

        for (guint i = 0; i < limit; ++i) {
            const GValue* val = gst_value_list_get_value(magnitudes, i);
            float db = -80.0f;
            if (G_VALUE_HOLDS_FLOAT(val)) {
                db = static_cast<float>(g_value_get_float(val));
            } else if (G_VALUE_HOLDS_DOUBLE(val)) {
                db = static_cast<float>(g_value_get_double(val));
            }
            if (!std::isfinite(db)) {
                db = -80.0f;
            }
            db = std::clamp(db, -80.0f, 0.0f);
            bars_[i] = (db + 80.0f) / 80.0f;
        }
    }

    GstElement* playbin_ = nullptr;
    GstElement* spectrum_ = nullptr;
    GstBus* bus_ = nullptr;

    std::vector<TrackInfo> tracks_;
    size_t index_ = 0;
    State state_ = State::Stopped;
    std::array<float, kSpecWidth> bars_{};
    std::string last_error_;
};

struct UiButtons {
    int y = 0;
    int x_prev = 0;
    int x_play = 0;
    int x_stop = 0;
    int x_next = 0;
};

std::string FormatDuration(double sec) {
    if (sec <= 0.0) {
        return "--:--";
    }
    int total = static_cast<int>(sec + 0.5);
    int m = total / 60;
    int s = total % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

void DrawBar(int y, int x, int h, int color_pair) {
    attron(COLOR_PAIR(color_pair));
    for (int i = 0; i < h; ++i) {
        mvaddstr(y - i, x, "█");
    }
    attroff(COLOR_PAIR(color_pair));
}

UiButtons DrawUi(const Player& p) {
    erase();

    const int cols = COLS;
    const int rows = LINES;
    const int panel_w = kSpecWidth;
    const int panel_h = kSpecHeight + 1;

    const int x0 = std::max(0, (cols - panel_w) / 2);
    const int y0 = std::max(0, (rows - panel_h) / 2);

    const auto& bars = p.bars();

    for (int x = 0; x < kSpecWidth; ++x) {
        int h = static_cast<int>(std::round(bars[x] * kSpecHeight));
        h = std::clamp(h, 0, kSpecHeight);

        int cp = 1;
        if (h > 7) {
            cp = 3;
        } else if (h > 3) {
            cp = 2;
        }

        if (h > 0) {
            DrawBar(y0 + kSpecHeight - 1, x0 + x, h, cp);
        }
    }

    UiButtons btn;
    btn.y = y0 + kSpecHeight;

    const wchar_t* prev = L"⏮";
    const wchar_t* play = p.is_playing() ? L"⏸" : L"▶";
    const wchar_t* stop = L"⏹";
    const wchar_t* next = L"⏭";

    constexpr int kBtnCellWidth = 1;
    constexpr int kGap = 2;
    constexpr int kButtonsTotalWidth = (kBtnCellWidth * 4) + (kGap * 3);
    int bx = std::max(0, (cols - kButtonsTotalWidth) / 2);

    attron(A_BOLD | COLOR_PAIR(4));
    mvaddwstr(btn.y, bx, prev);
    mvaddwstr(btn.y, bx + kBtnCellWidth + kGap, play);
    mvaddwstr(btn.y, bx + (kBtnCellWidth + kGap) * 2, stop);
    mvaddwstr(btn.y, bx + (kBtnCellWidth + kGap) * 3, next);
    attroff(A_BOLD | COLOR_PAIR(4));

    btn.x_prev = bx;
    btn.x_play = bx + kBtnCellWidth + kGap;
    btn.x_stop = bx + (kBtnCellWidth + kGap) * 2;
    btn.x_next = bx + (kBtnCellWidth + kGap) * 3;

    attron(COLOR_PAIR(5));
    mvprintw(y0 - 2, std::max(0, (cols - 40) / 2), "Track %zu/%zu  %-24s %s",
             p.index() + 1, p.count(), p.current_track().display_name.c_str(),
             FormatDuration(p.current_track().duration_sec).c_str());
    mvprintw(y0 - 1, std::max(0, (cols - 40) / 2), "State: %-8s  Keys: Space < > s q", p.state_text().c_str());
    attroff(COLOR_PAIR(5));

    if (!p.last_error().empty()) {
        attron(COLOR_PAIR(3));
        mvprintw(btn.y + 2, std::max(0, (cols - static_cast<int>(p.last_error().size()) - 7) / 2),
                 "Error: %s", p.last_error().c_str());
        attroff(COLOR_PAIR(3));
    }

    refresh();
    return btn;
}

bool InButton(int mx, int my, int bx, int by, int w = 1) {
    return my == by && mx >= bx && mx < bx + w;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <audio files...>\n";
        return 1;
    }

    gst_init(&argc, &argv);
    avformat_network_init();

    std::vector<TrackInfo> tracks;
    tracks.reserve(argc - 1);

    for (int i = 1; i < argc; ++i) {
        auto info = FfmpegProbe::Probe(argv[i]);
        if (info) {
            tracks.push_back(*info);
        } else {
            TrackInfo fallback;
            fallback.path = argv[i];
            fallback.display_name = argv[i];
            tracks.push_back(std::move(fallback));
        }
    }

    try {
        setlocale(LC_ALL, "");

        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);
        curs_set(0);

        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, nullptr);

        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(1, COLOR_CYAN, -1);
            init_pair(2, COLOR_YELLOW, -1);
            init_pair(3, COLOR_RED, -1);
            init_pair(4, COLOR_GREEN, -1);
            init_pair(5, COLOR_WHITE, -1);
        }

        Player player(std::move(tracks));
        player.toggle_play_pause();

        bool quit = false;
        while (!quit) {
            player.update();
            UiButtons btn = DrawUi(player);

            int ch = getch();
            switch (ch) {
                case 'q':
                case 'Q':
                    quit = true;
                    break;
                case ' ':
                    player.toggle_play_pause();
                    break;
                case '<':
                    player.prev_track();
                    break;
                case '>':
                    player.next_track();
                    break;
                case 's':
                case 'S':
                    player.stop();
                    break;
                case KEY_MOUSE: {
                    MEVENT ev;
                    if (getmouse(&ev) == OK && (ev.bstate & BUTTON1_CLICKED)) {
                        if (InButton(ev.x, ev.y, btn.x_prev, btn.y)) {
                            player.prev_track();
                        } else if (InButton(ev.x, ev.y, btn.x_play, btn.y)) {
                            player.toggle_play_pause();
                        } else if (InButton(ev.x, ev.y, btn.x_stop, btn.y)) {
                            player.stop();
                        } else if (InButton(ev.x, ev.y, btn.x_next, btn.y)) {
                            player.next_track();
                        }
                    }
                    break;
                }
                default:
                    break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        endwin();
    } catch (const std::exception& ex) {
        endwin();
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
