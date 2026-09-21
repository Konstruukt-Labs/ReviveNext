#pragma once

#define REV_MAKESTR(x) #x
#define REV_STRINGIFY(x) REV_MAKESTR(x)

#define REV_MAJOR_VERSION 3
#define REV_MINOR_VERSION 2
#define REV_PATCH_VERSION 0
#define REV_BUILD_NUMBER  0

#define REV_VERSION_STRING REV_STRINGIFY(REV_MAJOR_VERSION.REV_MINOR_VERSION.REV_PATCH_VERSION.REV_BUILD_NUMBER)
#define REV_VERSION_INT (REV_MAJOR_VERSION << 24 | REV_MINOR_VERSION << 16 | REV_PATCH_VERSION << 8 | REV_BUILD_NUMBER)

// Point this at a raw text file whose first line is a version string and
// (optional) second line is a release page URL, e.g. a raw.githubusercontent.com
// URL to an "update.txt" in your own fork/releases. Left empty, the manual
// "Check for updates" tray menu item reports that no URL is configured
// rather than silently doing nothing.
#define REV_UPDATE_URL ""
