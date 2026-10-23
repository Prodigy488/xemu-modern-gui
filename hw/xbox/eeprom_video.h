/*
 * Xbox EEPROM video settings
 *
 * The dashboard stores the user's video modes in the EEPROM user section
 * (offset 0x94). Games read them to decide whether to render widescreen or
 * in an HD mode.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#ifndef HW_XBOX_EEPROM_VIDEO_H
#define HW_XBOX_EEPROM_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

#define XBOX_EEPROM_VIDEO_WIDESCREEN 0x00010000
#define XBOX_EEPROM_VIDEO_720P       0x00020000
#define XBOX_EEPROM_VIDEO_1080I      0x00040000
#define XBOX_EEPROM_VIDEO_480P       0x00080000
#define XBOX_EEPROM_VIDEO_LETTERBOX  0x00100000

/* Read or change the video flags of the running machine's EEPROM. A change
 * updates the user section checksum and is saved to the EEPROM file; games
 * see it the next time they start. Both return false if there is no EEPROM
 * (machine not created yet). Call with the BQL held. */
bool xbox_eeprom_get_video_flags(uint32_t *flags);
bool xbox_eeprom_set_video_flags(uint32_t flags);

#endif
