//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "common.hh"
#include "actions.hh"
#include "misc.hh"
#include "xemu-hud.h"
#include "../xemu-snapshots.h"
#include "../xemu-notifications.h"
#include "snapshot-manager.hh"
#include "game-library.hh"
#include "folder-disc.hh"
#include <filesystem>

void ActionEjectDisc(void)
{
    if (GameLibraryInterceptEject()) {
        return; // saving a resume point first; the library ejects after
    }
    ActionEjectDiscNow();
}

void ActionEjectDiscNow(void)
{
    Error *err = NULL;
    xemu_eject_disc(&err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}

void ActionLoadDisc(void)
{
    static const SDL_DialogFileFilter filters[] = {
        { "Disc Image Files (*.iso, *.xiso)", "iso;xiso" },
        { "All Files", "*" }
    };
    const char *default_path = g_config.sys.files.dvd_path;
    if (!default_path || !default_path[0]) {
        default_path = g_config.general.games_dir;
    }
    ShowOpenFileDialog(filters, 2, default_path, [](const char *path) {
        ActionLoadDiscFile(path);
    });
}

void ActionLoadGameFolder(void)
{
    // A folder holding default.xbe and the rest of a game's files
    const char *default_path = g_config.general.games_dir;
    ShowOpenFolderDialog(default_path, [](const char *path) {
        ActionLoadDiscFile(path);
    });
}

void ActionLoadDiscFile(const char *file_path)
{
    // A game folder plays from a disc image built from it, once
    if (FolderDiscNeedsPrepare(file_path)) {
        std::string folder = file_path;
        std::string name = std::filesystem::u8path(folder).filename().u8string();
        FolderDiscPrepare(folder, name,
                          [folder] { ActionLoadDiscFile(folder.c_str()); });
        return;
    }
    Error *err = NULL;
    xemu_load_disc(file_path, &err);

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    } else {
        const char *games_dir = g_config.general.games_dir;
        if (!games_dir || !games_dir[0]) {
            std::string dir = std::filesystem::path(file_path).parent_path().string();
            xemu_settings_set_string(&g_config.general.games_dir, dir.c_str());
            xemu_settings_save();
        }
    }
}

void ActionTogglePause(void)
{
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    } else {
        vm_start();
    }
}

void ActionReset(void)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

void ActionShutdown(void)
{
    if (xemu_hud_intercept_quit()) {
        return; // saving a resume point first; the library quits after
    }
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
}

void ActionScreenshot(void)
{
	g_screenshot_pending = true;
}

void ActionActivateBoundSnapshot(int slot, bool save)
{
    assert(slot < 4 && slot >= 0);
    const char *snapshot_name = *(g_snapshot_shortcut_index_key_map[slot]);
    if (!snapshot_name || !(snapshot_name[0])) {
        char *msg = g_strdup_printf(tr("F%d is not bound to a snapshot"), slot + 5);
        xemu_queue_notification(msg);
        g_free(msg);
        return;
    }

    Error *err = NULL;
    if (save) {
        xemu_snapshots_save(snapshot_name, &err);
    } else {
        ActionLoadSnapshotChecked(snapshot_name);
    }

    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
    }
}

void ActionLoadSnapshotChecked(const char *name)
{
    g_snapshot_mgr.LoadSnapshotChecked(name);
}
