//
// xemu User Interface - turning songs into what the Xbox dashboard plays
//
// The dashboard plays custom soundtracks as WMA version 2 (Windows Media
// Audio V8), 128 kbps, 44.1 kHz stereo. Songs in other formats (mp3, flac,
// ogg, m4a, wav, ...) are converted with ffmpeg when it can be found, or
// with Windows' own Media Foundation on Windows.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

// ffmpeg: the chosen one, or one next to xemu or on the PATH. "" if none.
std::string SoundtrackFindFfmpeg();
// Remember a chosen ffmpeg (empty to forget it)
void SoundtrackSetFfmpeg(const std::string &path);
std::string SoundtrackChosenFfmpeg();

// Can songs be converted here, and with what ("ffmpeg", "Windows Media
// Foundation"); "" in *how if not
bool SoundtrackCanConvert(std::string *how);

// Convert in (any audio file) to out (a .wma). Blocking; run it off the main
// thread. Stops early if *cancel becomes true.
bool SoundtrackConvert(const std::string &in, const std::string &out, std::string *err,
                       const std::atomic<bool> *cancel);

// Length of a .wma written by SoundtrackConvert
bool SoundtrackWmaLengthMs(const std::string &path, uint32_t *ms);

// The song's title: the tag in an mp3, else the file name without its extension
std::string SoundtrackTitleOf(const std::string &path);
