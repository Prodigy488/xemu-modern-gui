//
// xemu User Interface - achievements
//
// xemu's own achievements: milestones in xemu itself (first game, hours
// played, save states, homebrew...), kept in achievements.json next to the
// library. They are not in-game achievements of any Xbox game.
//
// Also the RetroAchievements account sign-in. RetroAchievements has no
// original Xbox games yet, so the account is only shown for now.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <cstdint>
#include <string>

// Things that happen in xemu. Each one adds to a count that achievements
// look at.
enum AchEvent {
    kAchLaunch,       // a game started from the library (arg: title ID)
    kAchSaveState,    // a save state made by hand
    kAchQuickSave,
    kAchQuickLoad,
    kAchBackup,       // game saves backed up to the PC
    kAchHomebrew,     // homebrew installed from the store
    kAchDriveEdit,    // files changed in the Drive tab
    kAchSixtyFps,     // 60 FPS switched on
    kAchPatches,      // a patch file downloaded
    kAchInsigniaSetup,
    kAchOnlinePlay,
    kAchMouse,        // the mouse captured for play
    kAchKeyboard,     // the on-screen keyboard used
    kAchCollapse,     // the dashboard minimized
    kAchTab,          // a dashboard tab opened (arg: tab number)
};

// Where achievements.json lives; loads it. Call once at startup.
void AchievementsLoad(const std::string &path);
// Writes it now if anything changed
void AchievementsSave(void);
// Waits for the sign-in thread
void AchievementsShutdown(void);

void AchievementsEvent(AchEvent e, uint32_t arg = 0);
// Numbers xemu already keeps: "play_s" (seconds played, all games), "library"
// (games), "favorites". Only ever raised.
void AchievementsSetStat(const char *key, int64_t value);
// Every frame. filtering_crt: the CRT or Scanlines filter is on while a game
// runs; language: the interface language code ("" = English)
void AchievementsUpdate(bool filtering_crt, const char *language);
// The pop-ups in the lower right, over everything
void AchievementsDraw(void);
// Whether unlocks show a pop-up
bool AchievementsPopups(void);
void AchievementsSetPopups(bool on);
// Shows the pop-up for achievement i (for trying it out)
void AchievementsPreviewToast(int i);

struct AchievementInfo {
    const char *id;
    const char *name;     // English; run through tr() to show
    const char *desc;
    const char *category;
    int points;
    int64_t progress;     // how far, up to target
    int64_t target;
    int64_t unlocked_at;  // unix time, 0 = not yet
    bool unlocked;
};
int AchievementCount(void);
AchievementInfo AchievementAt(int i);
int AchievementsUnlocked(void);
int AchievementsPoints(void);
int AchievementsPointsTotal(void);

// RetroAchievements account
enum RaState { kRaSignedOut, kRaWorking, kRaSignedIn, kRaFailed };
struct RaInfo {
    RaState state = kRaSignedOut;
    std::string user;
    std::string error;    // when state is kRaFailed
    int64_t points = 0;   // hardcore
    int64_t softcore = 0;
};
RaInfo RaGetInfo(void);
// Signs in with a password. Only the token RetroAchievements sends back is
// kept; the password is not saved.
void RaSignIn(const std::string &user, const std::string &password);
// Checks the saved token again (once per run, when the tab is opened)
void RaRefresh(void);
void RaSignOut(void);

// For testing: reads RetroAchievements' login reply. Returns true on success.
bool RaParseLogin(const std::string &json, RaInfo &out, std::string &token);
