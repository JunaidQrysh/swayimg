// SPDX-License-Identifier: MIT
// Embedded video playback (libmpv rendering into OpenGL texture).

#pragma once

#include "geometry.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

struct mpv_handle;
struct mpv_render_context;

class EmbeddedVideo {
public:
    /**
     * Check if embedded playback is available (libmpv and GPU rendering).
     * @return true if videos can be played inside the window
     */
    static bool available();

    EmbeddedVideo() = default;
    ~EmbeddedVideo();

    EmbeddedVideo(const EmbeddedVideo&) = delete;
    EmbeddedVideo& operator=(const EmbeddedVideo&) = delete;

    /**
     * Start playback (player instance is reused between files).
     * @param path path to the video file
     * @return false on errors
     */
    bool start(const std::filesystem::path& path);

    /**
     * Stop playback, player instance stays initialized.
     */
    void stop();

    /**
     * Initialize player and renderer in advance to start playback without
     * delay later. GL context must be current.
     * @return false on errors
     */
    bool prepare();

    /**
     * Check if renderer is initialized (playback can start immediately).
     * @return true if render context exists
     */
    [[nodiscard]] bool ready() const { return render_ctx; }

    /**
     * Render player output (video frame and on-screen controller) to the
     * texture of window size, GL context must be current.
     * @param size texture (window) size in pixels
     * @param x,y,w,h video area on the window
     * @return GL texture (bottom-up, premultiplied alpha, transparent outside
     *         the video area), 0 if no frame yet
     */
    unsigned render(const Size& size, const double x, const double y,
                    const double w, const double h);

    /**
     * Move mouse pointer (for on-screen controller).
     * @param x,y position on the window in pixels
     */
    void mouse_move(const ssize_t x, const ssize_t y);

    /**
     * Press/release mouse button (for on-screen controller).
     * @param name mpv button name (MBTN_LEFT etc)
     * @param pressed true if pressed, false if released
     */
    void mouse_button(const char* name, const bool pressed);

    /** Toggle pause. */
    void toggle_pause();

    /**
     * Seek relative to the current position.
     * @param seconds seek distance
     */
    void seek(const double seconds);

    /**
     * Change volume.
     * @param delta volume change in percent
     */
    void change_volume(const double delta);

    /**
     * Get playback progress.
     * @return position in range [0,1]
     */
    [[nodiscard]] double progress();

    /**
     * Check if playback is paused.
     * @return true if paused
     */
    [[nodiscard]] bool paused();

private:
    /** Create and initialize mpv instance. */
    bool init();

    /** Drain mpv event queue. */
    void process_events();

    /** Create render context, GL context must be current. */
    bool create_render_context();

    /** Set pause state of the player. */
    void set_pause(const bool pause);

    /** Load file paused, playback starts when its first frame is shown. */
    void load(const std::string& file);

    static void on_update(void* ctx);

    mpv_handle* mpv = nullptr;
    mpv_render_context* render_ctx = nullptr;
    bool render_failed = false;
    unsigned fbo = 0;
    unsigned texture = 0;
    Size texture_size;
    double zoom = 0;  ///< Current video-zoom property value
    double pan_x = 0; ///< Current video-pan-x property value
    double pan_y = 0; ///< Current video-pan-y property value
    bool playing = false;    ///< Playback is active
    bool frame_seen = false; ///< At least one video frame decoded
    bool started = false;    ///< Playback of the current file started
    bool hold = false;       ///< Paused until the first frame is shown
    double percent_pos = 0;  ///< Playback position (observed property)
    bool is_paused = false;  ///< Pause state (observed property)
    std::string pending;     ///< File to load when renderer is ready
    std::atomic<bool> update = false;
};
