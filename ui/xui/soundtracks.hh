//
// xemu User Interface - custom soundtracks for the Xbox dashboard
//
// A window to pick songs (mp3, flac, ogg, m4a, wav, wma...), convert them to
// the WMA the dashboard plays, and add them to the emulated Xbox's hard drive
// as a new soundtrack or to one it already has. They then show up under Music
// in the Xbox dashboard and can be played in games that allow custom
// soundtracks.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

void SoundtracksOpenWindow(void);
// Every frame: moves conversion along and finishes the copy to the Xbox
void SoundtracksUpdate(void);
void SoundtracksWindowDraw(void);
// The screen itself, for the dashboard's Soundtracks tab
void SoundtracksDrawContent(void);
