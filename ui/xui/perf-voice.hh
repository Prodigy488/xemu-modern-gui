//
// xemu User Interface - voice commands for Performance Capture
//
// Uses the speech recognizer built into Windows, with a grammar of three
// fixed phrases, on the PC: nothing is sent anywhere. Off by default; the
// microphone is only opened while it's on.
//
//   "performance capture start"  start recording (until told to stop)
//   "performance capture stop"   stop and show the report
//   "performance capture mark"   mark a glitch
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

enum class PerfVoiceCommand { None, Start, Stop, Mark };

// Turns listening on or off (cheap to call every frame)
void PerfVoiceSetEnabled(bool enabled);
// The next command heard, if any
PerfVoiceCommand PerfVoicePoll();
// What it's doing, for the settings: "Listening", or why it can't.
// *ok is false when it can't listen.
std::string PerfVoiceStatus(bool *ok);
// True while the microphone is open
bool PerfVoiceListening();
// Stops the listener (at exit)
void PerfVoiceShutdown();
