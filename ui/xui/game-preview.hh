//
// xemu User Interface - Game library preview videos
//
// Plays a user-provided preview video (MP4, WMV, ...) muted and looping in
// the game library. Decoding uses Windows Media Foundation on a background
// thread; the main thread uploads the newest frame to a GL texture. On other
// platforms previews are unavailable.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

class PreviewPlayer {
public:
    PreviewPlayer();
    ~PreviewPlayer();

    // Start playing `path` (UTF-8). Stops any current video.
    void Open(const std::string &path);
    // Stop playback. Pass false at exit, when the GL context may be gone.
    void Close(bool release_gl = true);

    bool IsOpen() const;
    const std::string &Path() const;

    // Main thread, once per frame: uploads a new frame if one is ready.
    // Returns the GL texture (0 until the first frame) and its size.
    unsigned int Update(int *w, int *h);

    // Non-empty if the video could not be played
    std::string Error() const;

    static bool Supported();

private:
    struct Impl;
    Impl *m;
};

// File types offered when importing a preview
extern const char *const kPreviewExtensions[];
