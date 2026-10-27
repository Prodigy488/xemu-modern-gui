//
// xemu User Interface - achievements
//
// Achievements are per game. The original Xbox had none, so they come from
// pack files: small JSON files in an "achievements" folder, one or more per
// game, named after the game's title ID (e.g. 4D530004.json). Anyone can
// write and share them, and you can add your own from the Achievements tab
// (those are kept in <TITLEID>-mine.json).
//
// xemu can't see inside a game, so you tick achievements off yourself. A
// pop-up slides in at the lower right when you do.
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
#include <vector>

// Where the progress file is and where the packs are; loads both. Call once
// at startup.
void AchievementsLoad(const std::string &progress_path, const std::string &packs_dir);
// Reads the pack files again (after adding or changing one)
void AchievementsReloadPacks(void);
// Writes the progress now if anything changed
void AchievementsSave(void);
// Waits for the sign-in thread
void AchievementsShutdown(void);
// Every frame: saves a little after something changes
void AchievementsUpdate(void);
// The pop-ups in the lower right, over everything
void AchievementsDraw(void);
// Whether unlocking shows a pop-up
bool AchievementsPopups(void);
void AchievementsSetPopups(bool on);

struct AchievementInfo {
    std::string id;
    std::string name;
    std::string desc;
    std::string group;   // "" = none
    int points = 0;
    bool secret = false; // name and text hidden until done
    bool mine = false;   // made here, can be edited
    bool done = false;
    int64_t done_at = 0; // unix time
};

// Games that have achievements, by title ID
std::vector<uint32_t> AchievementsGames(void);
// The name a pack gives the game ("" if none)
std::string AchievementsGameName(uint32_t title_id);
// Who wrote the packs for this game (names joined with ", ")
std::string AchievementsAuthors(uint32_t title_id);
int AchievementCount(uint32_t title_id);
AchievementInfo AchievementAt(uint32_t title_id, int i);
int AchievementsDoneFor(uint32_t title_id);
int AchievementsPointsFor(uint32_t title_id);       // earned
int AchievementsPointsTotalFor(uint32_t title_id);  // possible
// Everything, across all games
int AchievementsDoneAll(void);
int AchievementsCountAll(void);
int AchievementsPointsAll(void);

// Ticks one off (or back on). Shows the pop-up when it becomes done.
void AchievementsSet(uint32_t title_id, const std::string &id, bool done);
// Shows the pop-up for the first achievement of a game (for trying it out)
void AchievementsPreviewToast(uint32_t title_id);

// Your own achievements for a game. game_name is kept in the file so the
// pack still says which game it is for. Return the new id, or "" when it
// couldn't be saved.
std::string AchievementsAddMine(uint32_t title_id, const std::string &game_name,
                                const std::string &name, const std::string &desc,
                                int points, const std::string &group);
bool AchievementsEditMine(uint32_t title_id, const std::string &id, const std::string &name,
                          const std::string &desc, int points, const std::string &group);
bool AchievementsDeleteMine(uint32_t title_id, const std::string &id);

// The packs folder (made if it's missing)
std::string AchievementsPacksDir(void);
// For testing: reads one pack's JSON. Returns the number of achievements
// read, or -1 if it isn't a usable pack.
int AchievementsParsePack(const std::string &json, uint32_t file_title_id);

// Community packs: a list kept in this project's repository that anyone can
// add to. The list and the packs come from other people, so every file name
// and value is checked before anything is written.
struct CommunityPack {
    std::string file;    // name of the pack file, e.g. 4D530004-example.json
    uint32_t title_id = 0;
    std::string game;
    std::string author;
    std::string note;
    int count = 0;       // how many achievements it says it has
};
// Reads the list. False if it isn't a usable list.
bool AchievementsParseCommunityIndex(const std::string &json, std::vector<CommunityPack> &out);
// Whether a pack file name is safe to use (letters, digits, . _ - only)
bool AchievementsPackFileNameOk(const std::string &name);
// Checks a downloaded pack and saves it in the packs folder, then reloads.
// "" if it worked, otherwise what went wrong.
std::string AchievementsInstallPack(const CommunityPack &pack, const std::string &json);
bool AchievementsPackInstalled(const CommunityPack &pack);

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
