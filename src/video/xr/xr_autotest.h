/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_autotest.h Unattended device test of the Meta Quest build (see xr_autotest.cpp). */

#ifndef VIDEO_XR_AUTOTEST_H
#define VIDEO_XR_AUTOTEST_H

void XrAutotestEnable();
/** Advance the test; called once per frame from the video driver with the game state locked. */
void XrAutotestStep();

#endif /* VIDEO_XR_AUTOTEST_H */
