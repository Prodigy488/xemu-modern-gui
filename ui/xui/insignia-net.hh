//
// xemu User Interface - Insignia network check
//
// Asks a DNS server for an Xbox Live address over UDP, the way the Xbox
// would through xemu's NAT. Used to check that Insignia's DNS answers from
// this PC (some networks redirect or block DNS).
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once
#include <string>

// The name the check looks up: Xbox Live's machine account service, which
// only Insignia's DNS answers
#define INSIGNIA_TEST_NAME "macs.xboxlive.com"

enum class DnsQueryResult { Answer, NoAnswer, NoReply, Error };

// One A-record lookup of name at server:port (blocking, up to timeout_ms).
// *ip gets the first address on Answer; *err says what went wrong.
DnsQueryResult DnsQueryA(const char *server, int port, const char *name,
                         int timeout_ms, std::string *ip, std::string *err);

// Background check of Insignia's DNS, for the Insignia tab
void InsigniaDnsTestStart();
// 0 not run, 1 running, 2 passed, 3 failed; *msg says what happened
int InsigniaDnsTestState(std::string *msg);
