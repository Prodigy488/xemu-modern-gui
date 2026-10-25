//
// xemu User Interface - Game library
//
// A browsable collection of the disc images in the games directory, with
// title metadata and icons read from each disc, cover art, favorites,
// play time tracking, recently played games and per-game settings.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct RecentGame {
    std::string path;
    std::string title;
};

class GameLibraryWindow {
public:
    bool is_open = false;

    void Open();
    void ToggleOpen();
    // Hide it, leaving a small tab at the bottom of the screen to bring it
    // back
    void Collapse();

    // Called once per frame from the HUD update, before Draw(). Tracks the
    // loaded disc for play time and applies per-game settings.
    void Update();
    void Draw();

    // Most recently played games, newest first
    std::vector<RecentGame> GetRecentGames(int max_count);
    void Launch(const std::string &path);
    void Rescan();
    void OpenFolderManager();
    void AddGame();
    void DownloadCovers();
    // Online patches (Jay's Magic Patches) for every game in the library
    void DownloadPatches();
    void CheckNewPatches();
    void OpenInsignia();
};

extern GameLibraryWindow game_library_window;

// Keep per-game setting overrides out of the saved global config. Called by
// xemu_settings_save() around writing the config file.
void GameLibraryBeforeSettingsSave(void);

// Opens xemu's full settings (defined with the main menu)
void GameLibraryShowAllSettings(void);

// Called by ActionEjectDisc(): returns true if the library will save a
// resume point and then eject itself.
bool GameLibraryInterceptEject(void);
void GameLibraryAfterSettingsSave(void);

// Quick save (or load) for the game in the drive: one snapshot per game,
// from the controller (Back + LB / Back + RB)
void GameLibraryQuickSave(bool load);

// Where game save backups go on the PC (a folder per backup)
std::string GameLibrarySaveBackupDir(void);
// A dialog used Esc / B this frame, so the library shouldn't also close
void GameLibraryConsumeBack(void);
// The setup check (MCPX, BIOS, hard disk, games folder) on the dashboard
void GameLibraryOpenSetup(void);
// Name of the game in the DVD drive ("" if none)
std::string GameLibraryCurrentTitle(void);
// Name of a game by title ID, from your library or xemu's compatibility list
// ("" if unknown)
std::string GameLibraryTitleName(uint32_t title_id);

// The dashboard theme's colors, for other UI drawn to match (the on-screen
// keyboard). neon: cut corners; otherwise rounded.
struct GameLibraryColors {
    bool neon;
    uint32_t accent, accent2, text, dim, on_accent; // ImU32 (ABGR)
};
GameLibraryColors GameLibraryThemeColors(void);

