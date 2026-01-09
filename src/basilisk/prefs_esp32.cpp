/*
 *  prefs_esp32.cpp - Preferences handling for ESP32
 *
 *  BasiliskII ESP32 Port
 */

#include "sysdeps.h"
#include "prefs.h"

#define DEBUG 0
#include "debug.h"

// Platform-specific preferences items
prefs_desc platform_prefs_items[] = {
    {NULL, TYPE_END, false, NULL}  // End marker
};

/*
 *  Load preferences from settings file
 */
void LoadPrefs(const char *vmdir)
{
    UNUSED(vmdir);
    
    Serial.println("[PREFS] Loading preferences...");
    
    // Set ROM file path
    PrefsReplaceString("rom", "/Q650.ROM");
    
    // Set model ID to Quadra 900 (14) for MacOS 8 compatibility
    // Quadra 650 is similar architecture
    PrefsReplaceInt32("modelid", 14);
    
    // Set CPU type to 68040
    PrefsReplaceInt32("cpu", 4);
    
    // Disable FPU (not implemented on ESP32)
    PrefsReplaceBool("fpu", false);
    
    // Set RAM size to 8MB
    PrefsReplaceInt32("ramsize", 8 * 1024 * 1024);
    
    // Set screen configuration
    PrefsReplaceString("screen", "win/640/480");
    
    // Add hard disk image (read-only for safety on ESP32)
    PrefsReplaceString("disk", "*/Macintosh.dsk");
    
    // Add floppy disk image (read-only for safe booting)
    // PrefsReplaceString("floppy", "*/DiskTools1.img");
    
    Serial.println("[PREFS] Disk: /Macintosh.dsk (read-only)");
    // Serial.println("[PREFS] Floppy: /DiskTools1.img (read-only)");
    
    // Disable sound (for now)
    PrefsReplaceBool("nosound", true);
    
    // Enable CD-ROM and mount System 7.5.3 ISO
    PrefsReplaceBool("nocdrom", false);
    PrefsReplaceString("cdrom", "/System753.iso");
    Serial.println("[PREFS] CD-ROM: /System753.iso");
    
    // No GUI
    PrefsReplaceBool("nogui", true);
    
    // Boot from first bootable volume
    PrefsReplaceInt32("bootdrive", 0);
    PrefsReplaceInt32("bootdriver", 0);
    
    // Frame skip (lower = smoother but slower)
    PrefsReplaceInt32("frameskip", 4);
    
    Serial.println("[PREFS] Preferences loaded");
    
    // Debug: Print loaded prefs
    D(bug("  ROM: %s\n", PrefsFindString("rom")));
    D(bug("  Model ID: %d\n", PrefsFindInt32("modelid")));
    D(bug("  CPU: %d\n", PrefsFindInt32("cpu")));
    D(bug("  RAM: %d bytes\n", PrefsFindInt32("ramsize")));
}

/*
 *  Save preferences to settings file (no-op on ESP32)
 */
void SavePrefs(void)
{
    // Preferences are hardcoded, no saving needed
}

/*
 *  Add default preferences items
 */
void AddPlatformPrefsDefaults(void)
{
    // Defaults are set in LoadPrefs
}
