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

#pragma once
#include <string>
#include <vector>
#include "../xemu-snapshots.h"

class SnapshotManager
{
public:
    QEMUSnapshotInfo *m_snapshots;
    XemuSnapshotData *m_extra_data;
    bool m_load_failed;
    bool m_open_pending;
    int m_snapshots_len;

    std::string m_pending_load_name;
    std::string m_current_disc_path;
    std::string m_target_disc_path;

    // Loading a save state older than changes to the Xbox hard drive would
    // undo them: asked first
    bool m_drive_open_pending = false;
    std::string m_drive_load_name, m_drive_disc_path;
    std::vector<std::string> m_drive_changes;

    SnapshotManager();
    ~SnapshotManager();
    void Refresh();
    void LoadSnapshot(const char *name);
    void LoadSnapshotChecked(const char *name);
    // After the hard drive question: the disc check, then load
    void LoadSnapshotCheckedDisc(const char *name);

    void Draw();
    void DrawSnapshotDiscLoadDialog();
    void DrawDriveChangesDialog();
};

extern SnapshotManager g_snapshot_mgr;
