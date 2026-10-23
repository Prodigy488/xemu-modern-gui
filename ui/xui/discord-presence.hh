//
// xemu User Interface - Discord Rich Presence
//
// Shows the game being played on the user's Discord profile. Talks to the
// local Discord app over its IPC socket (a named pipe on Windows), so no
// Discord SDK is needed. Does nothing if Discord isn't running.
//
// Pure C++17 with no emulator dependencies.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>

struct DiscordActivity {
    std::string details;     // first line, e.g. the game's name
    std::string state;       // second line
    std::string large_image; // asset key or https URL
    std::string large_text;
    std::string small_image; // asset key, optional
    std::string small_text;
    int64_t start = 0;       // unix time the activity started, 0 = none

    bool operator==(const DiscordActivity &o) const;
    bool operator!=(const DiscordActivity &o) const { return !(*this == o); }
};

// Start the background connection (idempotent). Reconnects on its own if
// Discord starts later or restarts.
void DiscordPresenceStart(const std::string &client_id);

// Set what to show; only sent when it changes. An empty activity (no details)
// clears the status.
void DiscordPresenceSet(const DiscordActivity &activity);

// Clear the status and stop the background thread.
void DiscordPresenceStop(void);

// For tests: the JSON payload SET_ACTIVITY would send for an activity.
std::string DiscordActivityJson(const DiscordActivity &activity, int64_t pid,
                                const std::string &nonce);

// For tests: where to connect (overrides the platform default when set).
void DiscordPresenceSetSocketPathForTest(const std::string &path);
bool DiscordPresenceConnected(void);
