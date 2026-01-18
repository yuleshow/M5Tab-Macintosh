/*
 *  main_esp32.cpp - Main program entry point for ESP32
 *
 *  BasiliskII ESP32 Port
 *
 *  Dual-core optimized:
 *  - Core 1: CPU emulation (main Arduino loop)
 *  - Core 0: Video rendering task, timer interrupts
 */

#include "sysdeps.h"

#include <M5Unified.h>
#include <M5GFX.h>
#include <SD.h>
#include <esp_heap_caps.h>

// FreeRTOS for dual-core support and timers
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "cpu_emulation.h"
#include "sys.h"
#include "rom_patches.h"
#include "xpram.h"
#include "timer.h"
#include "video.h"
#include "prefs.h"
#include "prefs_items.h"
#include "main.h"
#include "macos_util.h"
#include "user_strings.h"
#include "input.h"

#define DEBUG 1
#include "debug.h"

// ROM file size limits
const uint32 ROM_MIN_SIZE = 64 * 1024;    // 64KB minimum
const uint32 ROM_MAX_SIZE = 1024 * 1024;  // 1MB maximum

// Memory pointers (declared in basilisk_glue.cpp)
extern uint32 RAMBaseMac;
extern uint8 *RAMBaseHost;
extern uint32 RAMSize;
extern uint32 ROMBaseMac;
extern uint8 *ROMBaseHost;
extern uint32 ROMSize;

// Frame buffer pointers (defined in basilisk_glue.cpp)
extern uint8 *MacFrameBaseHost;
extern uint32 MacFrameSize;
extern int MacFrameLayout;

// CPU and FPU type
int CPUType = 4;           // 68040
bool CPUIs68060 = false;
int FPUType = 1;           // 68881
bool TwentyFourBitAddressing = false;

// Interrupt flags
uint32 InterruptFlags = 0;

// Forward declaration
void basilisk_loop(void);

// CPU tick counter for timing (used by newcpu.cpp)
// With video rendering offloaded to Core 0, we can use a much higher quantum
// Higher quantum = less frequent periodic checks = faster emulation
// Increased to 40000 with 15fps video for maximum emulation performance
int32 emulated_ticks = 40000;
static int32 emulated_ticks_quantum = 40000;

// ============================================================================
// IPS (Instructions Per Second) Monitoring
// ============================================================================
// These counters track emulated 68k instructions for performance measurement
static volatile uint64_t ips_total_instructions = 0;    // Total instructions executed
static volatile uint64_t ips_last_instructions = 0;     // Instructions at last report
static volatile uint32_t ips_last_report_time = 0;      // Time of last IPS report
static volatile uint32_t ips_current = 0;               // Most recent IPS measurement
#define IPS_REPORT_INTERVAL_MS 5000                     // Report IPS every 5 seconds

/*
 *  CPU tick check - called periodically during emulation
 *  
 *  This is called every emulated_ticks_quantum instructions (40000 by default).
 *  We use this to:
 *  1. Count instructions for IPS monitoring
 *  2. Handle periodic tasks (60Hz, video, input, etc.)
 */
void cpu_do_check_ticks(void)
{
    // Count instructions executed since last tick check
    // This is the number of instructions in one quantum
    ips_total_instructions += emulated_ticks_quantum;
    
    // Call basilisk_loop to handle periodic tasks
    basilisk_loop();
    
    // Reset tick counter
    emulated_ticks = emulated_ticks_quantum;
}

/*
 *  Report IPS (Instructions Per Second) statistics
 *  Called from basilisk_loop periodically
 */
static void reportIPSStats(uint32 current_time)
{
    if (current_time - ips_last_report_time >= IPS_REPORT_INTERVAL_MS) {
        uint64_t instructions_delta = ips_total_instructions - ips_last_instructions;
        uint32_t time_delta_ms = current_time - ips_last_report_time;
        
        if (time_delta_ms > 0) {
            // Calculate IPS (instructions per second)
            // Use 64-bit math to avoid overflow
            ips_current = (uint32_t)((instructions_delta * 1000ULL) / time_delta_ms);
            
            // Report in MIPS (millions of instructions per second) for readability
            float mips = ips_current / 1000000.0f;
            
            Serial.printf("[IPS] %u instructions/sec (%.2f MIPS), total: %llu\n", 
                          ips_current, mips, ips_total_instructions);
        }
        
        ips_last_instructions = ips_total_instructions;
        ips_last_report_time = current_time;
    }
}

/*
 *  Get current IPS measurement (for external use)
 */
uint32_t getEmulatorIPS(void)
{
    return ips_current;
}

/*
 *  Get total instructions executed (for external use)
 */
uint64_t getEmulatorTotalInstructions(void)
{
    return ips_total_instructions;
}

// Global emulator state
static bool emulator_running = false;
static uint32 last_60hz_time = 0;
static uint32 last_second_time = 0;
static uint32 last_video_signal = 0;
static uint32 last_disk_flush_time = 0;

// Video signal interval (ms) - how often to signal video task
// The video task runs at its own pace, this just triggers buffer swap
#define VIDEO_SIGNAL_INTERVAL 42  // ~24 FPS

// Disk flush interval (ms) - how often to flush write buffer to SD card
#define DISK_FLUSH_INTERVAL 2000  // 2 seconds

// FreeRTOS timer for 60Hz tick
static TimerHandle_t timer_60hz = NULL;

// NOTE: Input polling is now handled by a dedicated task on Core 0
// See input_esp32.cpp inputTask()

// ============================================================================
// Performance profiling counters for main loop
// ============================================================================
static uint32 perf_loop_count = 0;           // Number of basilisk_loop calls
static uint32 perf_flush_us = 0;             // Time spent in disk flush
static uint32 perf_flush_count = 0;          // Number of flushes
// NOTE: Input polling stats removed - input now runs on Core 0 task
static uint32 perf_main_last_report = 0;     // Last time stats were printed
#define PERF_MAIN_REPORT_INTERVAL_MS 5000    // Report every 5 seconds

/*
 *  Set/clear interrupt flags (thread-safe using atomic operations)
 */
void SetInterruptFlag(uint32 flag)
{
    // Use atomic OR for thread safety (called from timer callback on different core)
    __atomic_or_fetch(&InterruptFlags, flag, __ATOMIC_SEQ_CST);
}

void ClearInterruptFlag(uint32 flag)
{
    // Use atomic AND for thread safety
    __atomic_and_fetch(&InterruptFlags, ~flag, __ATOMIC_SEQ_CST);
}

/*
 *  Handle 60Hz tick - called from main loop at safe points
 *  Using polling instead of FreeRTOS timer to avoid race conditions
 */
static void handle_60hz_tick(void)
{
    // Set 60Hz interrupt flag
    SetInterruptFlag(INTFLAG_60HZ);
    
    // Handle ADB (mouse/keyboard) updates
    SetInterruptFlag(INTFLAG_ADB);
    
    // Trigger interrupt in CPU emulation
    TriggerInterrupt();
}

/*
 *  Start the 60Hz timer (now uses polling in main loop)
 */
static bool start60HzTimer(void)
{
    // No timer creation needed - we use polling now
    Serial.println("[MAIN] 60Hz using polling mode (safer)");
    return true;
}

/*
 *  Stop the 60Hz timer (no-op when using polling)
 */
static void stop60HzTimer(void)
{
    // No timer to stop in polling mode
}

/*
 *  Mutex functions (now using FreeRTOS primitives for thread safety)
 */
B2_mutex *B2_create_mutex(void)
{
    return new B2_mutex;
}

void B2_lock_mutex(B2_mutex *mutex)
{
    UNUSED(mutex);
}

void B2_unlock_mutex(B2_mutex *mutex)
{
    UNUSED(mutex);
}

void B2_delete_mutex(B2_mutex *mutex)
{
    delete mutex;
}

/*
 *  Flush code cache (no-op for interpreted emulation)
 */
void FlushCodeCache(void *start, uint32 size)
{
    UNUSED(start);
    UNUSED(size);
}

/*
 *  Display error alert
 */
void ErrorAlert(const char *text)
{
    Serial.printf("[ERROR] %s\n", text);
    
    // Also display on screen if possible
    M5.Display.fillScreen(TFT_RED);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println("BasiliskII Error:");
    M5.Display.println(text);
}

/*
 *  Display warning alert
 */
void WarningAlert(const char *text)
{
    Serial.printf("[WARNING] %s\n", text);
}

/*
 *  Display choice alert (always returns true on ESP32)
 */
bool ChoiceAlert(const char *text, const char *pos, const char *neg)
{
    Serial.printf("[CHOICE] %s (%s/%s)\n", text, pos, neg);
    return true;
}

/*
 *  Quit emulator
 */
void QuitEmulator(void)
{
    Serial.println("[MAIN] QuitEmulator called");
    emulator_running = false;
}

/*
 *  Load ROM file from SD card
 */
static bool LoadROM(const char *rom_path)
{
    Serial.printf("[MAIN] Loading ROM from: %s\n", rom_path);
    
    File rom_file = SD.open(rom_path, FILE_READ);
    if (!rom_file) {
        Serial.printf("[MAIN] ERROR: Cannot open ROM file: %s\n", rom_path);
        return false;
    }
    
    size_t rom_size = rom_file.size();
    Serial.printf("[MAIN] ROM file size: %d bytes\n", rom_size);
    
    if (rom_size < ROM_MIN_SIZE || rom_size > ROM_MAX_SIZE) {
        Serial.printf("[MAIN] ERROR: Invalid ROM size (expected %d-%d bytes)\n", 
                      ROM_MIN_SIZE, ROM_MAX_SIZE);
        rom_file.close();
        return false;
    }
    
    // Round up to nearest 64KB
    ROMSize = (rom_size + 0xFFFF) & ~0xFFFF;
    
    // Allocate ROM buffer in PSRAM
    ROMBaseHost = (uint8 *)ps_malloc(ROMSize);
    if (!ROMBaseHost) {
        Serial.println("[MAIN] ERROR: Cannot allocate ROM buffer in PSRAM!");
        rom_file.close();
        return false;
    }
    
    // Clear buffer
    memset(ROMBaseHost, 0, ROMSize);
    
    // Read ROM file
    size_t bytes_read = rom_file.read(ROMBaseHost, rom_size);
    rom_file.close();
    
    if (bytes_read != rom_size) {
        Serial.printf("[MAIN] ERROR: ROM read failed (got %d, expected %d)\n", 
                      bytes_read, rom_size);
        free(ROMBaseHost);
        ROMBaseHost = NULL;
        return false;
    }
    
    Serial.printf("[MAIN] ROM loaded successfully at %p (%d bytes)\n", 
                  ROMBaseHost, ROMSize);
    
    // Print first 16 bytes for debugging
    Serial.print("[MAIN] ROM header: ");
    for (int i = 0; i < 16; i++) {
        Serial.printf("%02X ", ROMBaseHost[i]);
    }
    Serial.println();
    
    return true;
}

/*
 *  Allocate Mac RAM
 */
static bool AllocateRAM(void)
{
    // Get RAM size from preferences
    RAMSize = PrefsFindInt32("ramsize");
    if (RAMSize < 1024 * 1024) {
        RAMSize = 8 * 1024 * 1024;  // Default 8MB
    }
    
    Serial.printf("[MAIN] Allocating %d bytes for Mac RAM...\n", RAMSize);
    
    // Allocate RAM in PSRAM
    RAMBaseHost = (uint8 *)ps_malloc(RAMSize);
    if (!RAMBaseHost) {
        Serial.println("[MAIN] ERROR: Cannot allocate Mac RAM in PSRAM!");
        return false;
    }
    
    // Clear RAM
    memset(RAMBaseHost, 0, RAMSize);
    
    Serial.printf("[MAIN] Mac RAM allocated at %p (%d bytes)\n", RAMBaseHost, RAMSize);
    
    return true;
}

/*
 *  1Hz tick handler
 */
static void handle_1hz_tick(void)
{
    SetInterruptFlag(INTFLAG_1HZ);
    TriggerInterrupt();
}

/*
 *  Initialize emulator
 */
static bool InitEmulator(void)
{
    Serial.println("\n========================================");
    Serial.println("  BasiliskII ESP32 - Macintosh Emulator");
    Serial.println("  Dual-Core Optimized Edition");
    Serial.println("========================================\n");
    
    // Print memory info including internal SRAM breakdown
    Serial.printf("[MAIN] Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MAIN] Free PSRAM: %d bytes\n", ESP.getFreePsram());
    Serial.printf("[MAIN] Total PSRAM: %d bytes\n", ESP.getPsramSize());
    
    // Report internal SRAM availability (critical for performance)
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    Serial.printf("[MAIN] Internal SRAM: %d/%d bytes free, largest block: %d bytes\n", 
                  free_internal, total_internal, largest_internal);
    
    Serial.printf("[MAIN] CPU Frequency: %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("[MAIN] Running on Core: %d\n", xPortGetCoreID());
    
    // Initialize preferences
    // PrefsInit expects references for argc/argv, but we don't have command line args
    // PrefsInit() internally calls LoadPrefs(), so we don't call it again
    int dummy_argc = 0;
    char *dummy_argv_data[] = { NULL };
    char **dummy_argv = dummy_argv_data;
    PrefsInit(NULL, dummy_argc, dummy_argv);
    
    // Initialize system I/O (SD card)
    SysInit();
    
    // Allocate Mac RAM
    if (!AllocateRAM()) {
        ErrorAlert("Failed to allocate Mac RAM");
        return false;
    }
    
    // Load ROM file
    const char *rom_path = PrefsFindString("rom");
    if (!rom_path) {
        rom_path = "/Q650.ROM";
    }
    
    if (!LoadROM(rom_path)) {
        ErrorAlert("Failed to load ROM file");
        return false;
    }
    
    // Initialize all emulator subsystems (including VideoInit which starts video task)
    Serial.println("[MAIN] Calling InitAll()...");
    if (!InitAll(NULL)) {
        ErrorAlert("InitAll() failed");
        return false;
    }
    
    // Start 60Hz FreeRTOS timer
    if (!start60HzTimer()) {
        // Non-fatal - will fall back to polling
        Serial.println("[MAIN] WARNING: 60Hz timer failed, using polling fallback");
    }
    
    // Initialize input handling (touch panel, USB keyboard/mouse)
    if (!InputInit()) {
        // Non-fatal - emulator can run without input
        Serial.println("[MAIN] WARNING: Input initialization failed");
    }
    
    Serial.println("[MAIN] Emulator initialized successfully!");
    Serial.printf("[MAIN] Tick quantum: %d instructions\n", emulated_ticks_quantum);
    
    // Print memory status after init
    Serial.printf("[MAIN] Free heap after init: %d bytes\n", ESP.getFreeHeap());
    Serial.printf("[MAIN] Free PSRAM after init: %d bytes\n", ESP.getFreePsram());
    
    // Report internal SRAM usage after all allocations
    size_t free_internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_internal_final = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[MAIN] Internal SRAM after init: %d/%d bytes free\n", 
                  free_internal_after, total_internal_final);
    Serial.printf("[MAIN] Internal SRAM used: %d bytes\n", 
                  total_internal_final - free_internal_after);
    
    return true;
}

/*
 *  Run emulator main loop
 */
static void RunEmulator(void)
{
    Serial.println("[MAIN] Starting 68k CPU emulation on Core 1...");
    Serial.println("[MAIN] Video rendering running on Core 0...");
    
    emulator_running = true;
    last_60hz_time = millis();
    last_second_time = millis();
    last_video_signal = millis();
    last_disk_flush_time = millis();
    
    // Start the 68k CPU - this function runs the emulation loop
    // It will return when QuitEmulator() is called
    Start680x0();
    
    Serial.println("[MAIN] 68k CPU emulation ended");
}

/*
 *  Arduino setup function
 */
void basilisk_setup(void)
{
    // Note: M5.begin() and Serial should already be initialized by main.cpp
    
    Serial.println("[MAIN] BasiliskII setup starting...");
    
    // Initialize emulator
    if (!InitEmulator()) {
        Serial.println("[MAIN] Emulator initialization failed!");
        
        // Display error and halt
        while (1) {
            delay(1000);
        }
    }
    
    // Run emulator
    RunEmulator();
    
    // Cleanup
    stop60HzTimer();
    InputExit();
    ExitAll();
    SysExit();
    PrefsExit();
    
    Serial.println("[MAIN] BasiliskII shutdown complete");
}

/*
 *  Report main loop performance stats periodically
 */
static void reportMainPerfStats(uint32 current_time)
{
    if (current_time - perf_main_last_report >= PERF_MAIN_REPORT_INTERVAL_MS) {
        perf_main_last_report = current_time;
        
        if (perf_loop_count > 0) {
            uint32 loops_per_sec = (perf_loop_count * 1000) / PERF_MAIN_REPORT_INTERVAL_MS;
            Serial.printf("[MAIN PERF] loops/sec=%u flushes=%u flush_avg=%uus\n",
                          loops_per_sec,
                          perf_flush_count,
                          perf_flush_count > 0 ? perf_flush_us / perf_flush_count : 0);
        }
        
        // Reset counters
        perf_loop_count = 0;
        perf_flush_us = 0;
        perf_flush_count = 0;
    }
}

/*
 *  Arduino loop function - called periodically during emulation
 *  This is called from the CPU emulator's main loop to handle periodic tasks
 *
 *  With dual-core optimization:
 *  - 60Hz tick is polled here (safer than async timer)
 *  - Video refresh is handled by video task on Core 0 (doesn't block here)
 *  - Input polling is handled by input task on Core 0 (doesn't block here)
 *  - This function is lightweight - no rendering or input polling happens here
 */
void basilisk_loop(void)
{
    uint32 current_time = millis();
    
    perf_loop_count++;
    
    // Handle 60Hz tick (~16ms intervals)
    if (current_time - last_60hz_time >= 16) {
        last_60hz_time = current_time;
        handle_60hz_tick();
    }
    
    // Handle 1Hz tick
    if (current_time - last_second_time >= 1000) {
        last_second_time = current_time;
        handle_1hz_tick();
    }
    
    // Signal video task that a new frame may be ready
    // This is non-blocking - just sets a flag for the video task to pick up
    if (current_time - last_video_signal >= VIDEO_SIGNAL_INTERVAL) {
        last_video_signal = current_time;
        VideoRefresh();  // Now just signals the video task, doesn't render
    }
    
    // Periodic disk write buffer flush (every 2 seconds)
    // Time check done here to avoid function call overhead on every tick
    if (current_time - last_disk_flush_time >= DISK_FLUSH_INTERVAL) {
        last_disk_flush_time = current_time;
        uint32 t0 = micros();
        Sys_periodic_flush();
        uint32 t1 = micros();
        perf_flush_us += (t1 - t0);
        perf_flush_count++;
    }
    
    // NOTE: Input polling (M5.update + InputPoll) is now handled by a dedicated
    // task on Core 0, removing ~2.3ms of blocking time from this loop.
    // See input_esp32.cpp inputTask()
    
    // Report performance stats periodically
    reportMainPerfStats(current_time);
    
    // Report IPS stats periodically
    reportIPSStats(current_time);
    
    // Yield to allow FreeRTOS tasks to run
    taskYIELD();
}

/*
 *  Check if emulator is running
 */
bool basilisk_is_running(void)
{
    return emulator_running;
}
