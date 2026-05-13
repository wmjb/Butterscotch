#include "data_win.h"
#include "vm.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "input_recording.h"
#include "debug_overlay.h"
#include "overlay_file_system.h"
#if defined(USE_OPENAL)
#include "al_audio_system.h"
#elif defined(USE_MINIAUDIO)
#include "ma_audio_system.h"
#endif
#include "noop_audio_system.h"

#include "utils.h"
#include "profiler.h"
#include "d3d9_renderer.h"
#include "stb_ds.h"
#include <windows.h>
#include <d3d9.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#ifdef __GLIBC__
#include <malloc.h>
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 33)
#define HAVE_MALLINFO2
#endif
#endif
#endif

// ===[ COMMAND LINE ARGUMENTS ]===
typedef struct {
    int key;
    // We need this dummy value, think that the ds_map is like a Java HashMap NOT a HashSet
    // (Which is funny, because in Java HashSets are backed by HashMaps lol)
    bool value;
} FrameSetEntry;

typedef struct {
    const char* dataWinPath;
    const char* saveFolder; // null = default to the directory containing dataWinPath
    const char* screenshotPattern;
    FrameSetEntry* screenshotFrames;
    const char* screenshotSurfacesPattern;
    FrameSetEntry* screenshotSurfacesFrames;
    FrameSetEntry* dumpFrames;
    FrameSetEntry* dumpJsonFrames;
    const char* dumpJsonFilePattern;
    StringBooleanEntry* varReadsToBeTraced;
    StringBooleanEntry* varWritesToBeTraced;
    StringBooleanEntry* functionCallsToBeTraced;
    StringBooleanEntry* alarmsToBeTraced;
    StringBooleanEntry* instanceLifecyclesToBeTraced;
    StringBooleanEntry* eventsToBeTraced;
    StringBooleanEntry* collisionsToBeTraced;
    StringBooleanEntry* opcodesToBeTraced;
    StringBooleanEntry* stackToBeTraced;
    StringBooleanEntry* disassemble;
    StringBooleanEntry* tilesToBeTraced;
    bool alwaysLogUnknownFunctions;
    bool alwaysLogStubbedFunctions;
    bool headless;
    bool traceFrames;
    bool printRooms;
    bool printDeclaredFunctions;
    int exitAtFrame;
    int traceBytecodeAfterFrame;
    double speedMultiplier;
    double fastForwardSpeed;
    int seed;
    bool hasSeed;
    bool debug;
    bool traceEventInherited;
    const char* recordInputsPath;
    const char* playbackInputsPath;
    const char* renderer; // kept for CLI compatibility, ignored for D3D9
    YoYoOperatingSystem osType;
    bool lazyRooms;
    StringBooleanEntry* eagerRooms; // stb_ds string-keyed set of room names
    int profilerFramesBetween; // 0 = disabled
#ifdef ENABLE_VM_OPCODE_PROFILER
    bool opcodeProfiler;
#endif
} CommandLineArgs;

typedef struct { const char* name; YoYoOperatingSystem value; } OsTypeNameEntry;

static const OsTypeNameEntry OS_TYPE_NAMES[] = {
    {"unknown",       OS_UNKNOWN},
    {"windows",       OS_WINDOWS},
    {"win32",         OS_WINDOWS},
    {"macosx",        OS_MACOSX},
    {"macos",         OS_MACOSX},
    {"psp",           OS_PSP},
    {"ios",           OS_IOS},
    {"android",       OS_ANDROID},
    {"symbian",       OS_SYMBIAN},
    {"linux",         OS_LINUX},
    {"winphone",      OS_WINPHONE},
    {"tizen",         OS_TIZEN},
    {"win8native",    OS_WIN8NATIVE},
    {"wiiu",          OS_WIIU},
    {"3ds",           OS_3DS},
    {"psvita",        OS_PSVITA},
    {"bb10",          OS_BB10},
    {"ps4",           OS_PS4},
    {"xboxone",       OS_XBOXONE},
    {"ps3",           OS_PS3},
    {"xbox360",       OS_XBOX360},
    {"uwp",           OS_UWP},
    {"amazon",        OS_AMAZON},
    {"switch",        OS_SWITCH},
};
#define OS_TYPE_NAMES_COUNT (sizeof(OS_TYPE_NAMES)/sizeof(OS_TYPE_NAMES[0]))

static bool parseOsTypeArg(const char* s, YoYoOperatingSystem* out) {
    forEach(const OsTypeNameEntry, entry, OS_TYPE_NAMES, OS_TYPE_NAMES_COUNT) {
        if (strcmp(s, entry->name) == 0) {
            *out = entry->value;
            return true;
        }
    }
    return false;
}

static void printOsTypeNames(FILE* out) {
    forEachIndexed(const OsTypeNameEntry, entry, i, OS_TYPE_NAMES, OS_TYPE_NAMES_COUNT) {
        fprintf(out, "%s%s", i > 0 ? ", " : "", entry->name);
    }
}

static void parseCommandLineArgs(CommandLineArgs* args, int argc, char* argv[]) {
    memset(args, 0, sizeof(CommandLineArgs));

    static struct option longOptions[] = {
        {"screenshot",          required_argument, nullptr, 's'},
        {"screenshot-at-frame", required_argument, nullptr, 'f'},
        {"screenshot-surfaces", required_argument, nullptr, 'U'},
        {"screenshot-surfaces-at-frame", required_argument, nullptr, 'V'},
        {"headless",            no_argument,       nullptr, 'h'},
        {"print-rooms", no_argument,               nullptr, 'r'},
        {"print-declared-functions", no_argument,  nullptr, 'p'},
        {"trace-variable-reads", required_argument,  nullptr, 'R'},
        {"trace-variable-writes", required_argument, nullptr, 'W'},
        {"trace-function-calls", required_argument,         nullptr, 'c'},
        {"trace-alarms", required_argument,         nullptr, 'a'},
        {"trace-instance-lifecycles", required_argument,         nullptr, 'l'},
        {"trace-events", required_argument,         nullptr, 'e'},
        {"trace-collisions", required_argument,     nullptr, 'C'},
        {"trace-event-inherited", no_argument, nullptr, 'E'},
        {"trace-tiles", required_argument, nullptr, 'T'},
        {"trace-opcodes", required_argument,       nullptr, 'o'},
        {"trace-stack", required_argument,         nullptr, 'S'},
        {"trace-frames", no_argument, nullptr, 'k'},
        {"always-log-unknown-functions", no_argument, nullptr, 'y'},
        {"always-log-stubbed-functions", no_argument, nullptr, 'Y'},
        {"exit-at-frame", required_argument, nullptr, 'x'},
        {"trace-bytecode-after-frame", required_argument, nullptr, 'F'},
        {"dump-frame", required_argument, nullptr, 'd'},
        {"dump-frame-json", required_argument, nullptr, 'j'},
        {"dump-frame-json-file", required_argument, nullptr, 'J'},
        {"speed", required_argument, nullptr, 'M'},
        {"fast-forward-speed", required_argument, nullptr, 'X'},
        {"seed", required_argument, nullptr, 'Z'},
        {"debug", no_argument, nullptr, 'D'},
        {"disassemble", required_argument, nullptr, 'A'},
        {"record-inputs", required_argument, nullptr, 'I'},
        {"playback-inputs", required_argument, nullptr, 'P'},
        {"renderer", required_argument, nullptr, 'g'},
        {"lazy-rooms", no_argument, nullptr, 'z'},
        {"eager-room", required_argument, nullptr, 'G'},
        {"os-type", required_argument, nullptr, 'O'},
        {"profile-gml-scripts", required_argument, nullptr, 'q'},
        {"save-folder", required_argument, nullptr, 'B'},
#ifdef ENABLE_VM_OPCODE_PROFILER
        {"profile-opcodes", no_argument, nullptr, 'Q'},
#endif
        {nullptr,               0,                 nullptr,  0 }
    };

    args->screenshotFrames = nullptr;
    args->exitAtFrame = -1;
    args->traceBytecodeAfterFrame = 0;
    args->speedMultiplier = 1.0;
    args->fastForwardSpeed = 0.0;
    args->renderer = "d3d9"; // kept only for CLI compatibility
    args->osType = OS_WINDOWS;
    args->profilerFramesBetween = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, "", longOptions, nullptr)) != -1) {
        switch (opt) {
            case 's':
                args->screenshotPattern = optarg;
                break;
            case 'f': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s'\n", optarg);
                    exit(1);
                }

                hmput(args->screenshotFrames, (int) frame, true);
                break;
            }
            case 'U':
                args->screenshotSurfacesPattern = optarg;
                break;
            case 'V': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --screenshot-surfaces-at-frame\n", optarg);
                    exit(1);
                }
                hmput(args->screenshotSurfacesFrames, (int) frame, true);
                break;
            }
            case 'h':
                args->headless = true;
                break;
            case 'r':
                args->printRooms = true;
                break;
            case 'p':
                args->printDeclaredFunctions = true;
                break;
            case 'R':
                shput(args->varReadsToBeTraced, optarg, true);
                break;
            case 'W':
                shput(args->varWritesToBeTraced, optarg, true);
                break;
            case 'c':
                shput(args->functionCallsToBeTraced, optarg, true);
                break;
            case 'a':
                shput(args->alarmsToBeTraced, optarg, true);
                break;
            case 'l':
                shput(args->instanceLifecyclesToBeTraced, optarg, true);
                break;
            case 'e':
                shput(args->eventsToBeTraced, optarg, true);
                break;
            case 'C':
                shput(args->collisionsToBeTraced, optarg, true);
                break;
            case 'o':
                shput(args->opcodesToBeTraced, optarg, true);
                break;
            case 'S':
                shput(args->stackToBeTraced, optarg, true);
                break;
            case 'k':
                args->traceFrames = true;
                break;
            case 'y':
                args->alwaysLogUnknownFunctions = true;
                break;
            case 'Y':
                args->alwaysLogStubbedFunctions = true;
                break;
            case 'x': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --exit-at-frame\n", optarg);
                    exit(1);
                }
                args->exitAtFrame = (int) frame;
                break;
            }
            case 'F': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --trace-bytecode-after-frame\n", optarg);
                    exit(1);
                }
                args->traceBytecodeAfterFrame = (int) frame;
                break;
            }
            case 'd': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame\n", optarg);
                    exit(1);
                }
                hmput(args->dumpFrames, (int) frame, true);
                break;
            }
            case 'j': {
                char* endPtr;
                long frame = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || 0 > frame) {
                    fprintf(stderr, "Error: Invalid frame number '%s' for --dump-frame-json\n", optarg);
                    exit(1);
                }
                hmput(args->dumpJsonFrames, (int) frame, true);
                break;
            }
            case 'J':
                args->dumpJsonFilePattern = optarg;
                break;
            case 'M': {
                char* endPtr;
                double speed = strtod(optarg, &endPtr);
                if (*endPtr != '\0' || speed <= 0.0) {
                    fprintf(stderr, "Error: Invalid speed multiplier '%s' for --speed (must be > 0)\n", optarg);
                    exit(1);
                }
                args->speedMultiplier = speed;
                break;
            }
            case 'X': {
                char* endPtr;
                double speed = strtod(optarg, &endPtr);
                if (*endPtr != '\0' || speed <= 0.0) {
                    fprintf(stderr, "Error: Invalid speed '%s' for --fast-forward-speed (must be > 0)\n", optarg);
                    exit(1);
                }
                args->fastForwardSpeed = speed;
                break;
            }
            case 'D':
                args->debug = true;
                break;
            case 'g':
                args->renderer = optarg; // accepted but ignored
                break;
            case 'z':
                args->lazyRooms = true;
                break;
            case 'G':
                shput(args->eagerRooms, optarg, true);
                break;
            case 'A':
                shput(args->disassemble, optarg, true);
                break;
            case 'T':
                shput(args->tilesToBeTraced, optarg, true);
                break;
            case 'E':
                args->traceEventInherited = true;
                break;
            case 'Z': {
                char* endPtr;
                long seedVal = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0') {
                    fprintf(stderr, "Error: Invalid seed value '%s' for --seed\n", optarg);
                    exit(1);
                }
                args->seed = (int) seedVal;
                args->hasSeed = true;
                break;
            }
            case 'I':
                args->recordInputsPath = optarg;
                break;
            case 'P':
                args->playbackInputsPath = optarg;
                break;
            case 'q': {
                char* endPtr;
                long framesBetween = strtol(optarg, &endPtr, 10);
                if (*endPtr != '\0' || framesBetween <= 0) {
                    fprintf(stderr, "Error: Invalid frame count '%s' for --profile-gml-scripts (must be > 0)\n", optarg);
                    exit(1);
                }
                args->profilerFramesBetween = (int) framesBetween;
                break;
            }
            case 'B':
                args->saveFolder = optarg;
                break;
#ifdef ENABLE_VM_OPCODE_PROFILER
            case 'Q':
                args->opcodeProfiler = true;
                break;
#endif
            case 'O':
                if (!parseOsTypeArg(optarg, &args->osType)) {
                    fprintf(stderr, "Error: Invalid --os-type value '%s' (expected: ", optarg);
                    printOsTypeNames(stderr);
                    fprintf(stderr, ")\n");
                    exit(1);
                }
                break;
            default:
                fprintf(stderr, "Usage: %s [--headless] [--screenshot=PATTERN] [--screenshot-at-frame=N ...] <path to data.win or game.unx>\n", argv[0]);
                exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Usage: %s [--headless] [--screenshot=PATTERN] [--screenshot-at-frame=N ...] <path to data.win or game.unx>\n", argv[0]);
        exit(1);
    }

    args->dataWinPath = argv[optind];

    if (hmlen(args->screenshotFrames) > 0 && args->screenshotPattern == nullptr) {
        fprintf(stderr, "Error: --screenshot-at-frame requires --screenshot to be set\n");
        exit(1);
    }

    if (hmlen(args->screenshotSurfacesFrames) > 0 && args->screenshotSurfacesPattern == nullptr) {
        fprintf(stderr, "Error: --screenshot-surfaces-at-frame requires --screenshot-surfaces to be set\n");
        exit(1);
    }

    if (args->headless && args->speedMultiplier != 1.0) {
        fprintf(stderr, "You can't set the speed multiplier while running in headless mode! Headless mode always run in real time\n");
        exit(1);
    }
}

static void freeCommandLineArgs(CommandLineArgs* args) {
    hmfree(args->screenshotFrames);
    hmfree(args->screenshotSurfacesFrames);
    hmfree(args->dumpFrames);
    hmfree(args->dumpJsonFrames);
    shfree(args->varReadsToBeTraced);
    shfree(args->varWritesToBeTraced);
    shfree(args->functionCallsToBeTraced);
    shfree(args->alarmsToBeTraced);
    shfree(args->instanceLifecyclesToBeTraced);
    shfree(args->eventsToBeTraced);
    shfree(args->collisionsToBeTraced);
    shfree(args->opcodesToBeTraced);
    shfree(args->stackToBeTraced);
    shfree(args->disassemble);
    shfree(args->tilesToBeTraced);
}

// ===[ INPUT RECORDING CRASH HANDLING ]===

static InputRecording* globalInputRecording = nullptr;

#if defined(__has_feature)
    #if __has_feature(address_sanitizer)
        #define BUTTERSCOTCH_HAS_ASAN 1
    #endif
#endif
#if defined(__SANITIZE_ADDRESS__)
    #define BUTTERSCOTCH_HAS_ASAN 1
#endif

#if BUTTERSCOTCH_HAS_ASAN
void __asan_set_death_callback(void (*callback)(void));
#endif

static volatile sig_atomic_t crashSaveInProgress = 0;

static void saveRecordingOnCrash(void) {
    if (crashSaveInProgress) return;
    crashSaveInProgress = 1;
    if (globalInputRecording != nullptr && globalInputRecording->isRecording) {
        InputRecording_save(globalInputRecording);
    }
}

static void crashSignalHandler(int sig) {
    saveRecordingOnCrash();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandlers(void) {
#if BUTTERSCOTCH_HAS_ASAN
    __asan_set_death_callback(saveRecordingOnCrash);
#endif
    signal(SIGSEGV, crashSignalHandler);
    signal(SIGABRT, crashSignalHandler);
#ifdef SIGBUS
    signal(SIGBUS,  crashSignalHandler);
#endif
    signal(SIGFPE,  crashSignalHandler);
    signal(SIGILL,  crashSignalHandler);
}

void saveInputRecording() {
    // Save input recording if active, then free
    if (globalInputRecording != nullptr) {
        if (globalInputRecording->isRecording) {
            InputRecording_save(globalInputRecording);
        }
        InputRecording_free(globalInputRecording);
        globalInputRecording = nullptr;
    }
}

#ifndef _WIN32
typedef struct { int key; struct sigaction value; } PreviousSignalActionEntry;
static PreviousSignalActionEntry* previousSignalActions = nullptr;

static void onCrashSignal(int sig) {
    saveInputRecording();
    // Restore the previous handler (ASAN) and re-raise so it can report the fault
    sigaction(sig, &previousSignalActions[hmgeti(previousSignalActions, sig)].value, nullptr);
    raise(sig);
}
#endif

// ===[ WIN32 + D3D9 ]===

static HWND g_window = NULL;
static bool g_windowShouldClose = false;
static Runner* g_runner = NULL;

static int32_t win32KeyToGml(WPARAM vk) {
    // Letters and numbers are the same as GML
    if (vk >= 'A' && vk <= 'Z') return (int32_t)vk;
    if (vk >= '0' && vk <= '9') return (int32_t)vk;
    // Special keys: just pass through VK_* codes, which match what the runner expects
    return (int32_t)vk;
}

static void setWin32WindowTitle(void* window, const char* title) {
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", title);
    HWND hwnd = (HWND)window;
    SetWindowTextA(hwnd, windowTitle);
}

static bool getWin32WindowFocus(void* window) {
    HWND hwnd = (HWND)window;
    return GetForegroundWindow() == hwnd;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            g_windowShouldClose = true;
            return 0;
        case WM_DESTROY:
            g_windowShouldClose = true;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) break;
            if (!g_runner) break;
            int32_t gmlKey = win32KeyToGml(wParam);
            if (gmlKey < 0) break;
            if (msg == WM_KEYDOWN) {
                RunnerKeyboard_onKeyDown(g_runner->keyboard, gmlKey);
            } else {
                RunnerKeyboard_onKeyUp(g_runner->keyboard, gmlKey);
            }
            break;
        }
        case WM_CHAR: {
            if (InputRecording_isPlaybackActive(globalInputRecording)) break;
            if (!g_runner) break;
            RunnerKeyboard_onCharacter(g_runner->keyboard, (uint32_t)wParam);
            break;
        }
        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static double nowTimeSeconds(void) {
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

// ===[ MAIN ]===
int main(int argc, char* argv[]) {
    CommandLineArgs args;
    parseCommandLineArgs(&args, argc, argv);

    printf("Loading %s...\n", args.dataWinPath);

    DataWin* dataWin = DataWin_parse(
        args.dataWinPath,
        (DataWinParserOptions) {
            .parseGen8 = true,
            .parseOptn = true,
            .parseLang = true,
            .parseExtn = false,
            .parseSond = true,
            .parseAgrp = true,
            .parseSprt = true,
            .parseBgnd = true,
            .parsePath = true,
            .parseScpt = true,
            .parseGlob = true,
            .parseShdr = true,
            .parseFont = true,
            .parseTmln = true,
            .parseObjt = true,
            .parseRoom = true,
            .parseTpag = true,
            .parseCode = true,
            .parseVari = true,
            .parseFunc = true,
            .parseStrg = true,
            .parseTxtr = true,
            .parseAudo = true,
            .skipLoadingPreciseMasksForNonPreciseSprites = true,
            .lazyLoadRooms = args.lazyRooms,
            .eagerlyLoadedRooms = args.eagerRooms
        }
    );

    Gen8* gen8 = &dataWin->gen8;
    printf("Loaded \"%s\" (%d) successfully! [Bytecode Version %u / GameMaker version %u.%u.%u.%u]\n",
           gen8->name, gen8->gameID, gen8->bytecodeVersion,
           dataWin->detectedFormat.major, dataWin->detectedFormat.minor,
           dataWin->detectedFormat.release, dataWin->detectedFormat.build);

#ifdef HAVE_MALLINFO2
    {
        struct mallinfo2 mi = mallinfo2();
        printf("Memory after data.win parsing: used=%zu bytes (%.1f KB)\n", mi.uordblks, mi.uordblks / 1024.0f);
    }
#endif

    // Build window title
    char windowTitle[256];
    snprintf(windowTitle, sizeof(windowTitle), "Butterscotch - %s", gen8->displayName);

    // Initialize VM
    VMContext* vm = VM_create(dataWin);

    Profiler_setEnabled(&vm->profiler, args.profilerFramesBetween > 0);
#ifdef ENABLE_VM_OPCODE_PROFILER
    vm->opcodeProfilerEnabled = args.opcodeProfiler;
    if (vm->opcodeProfilerEnabled) {
        vm->opcodeVariantCounts = safeCalloc(256 * 256, sizeof(uint64_t));
        vm->opcodeRValueTypeCounts = safeCalloc(256 * 256, sizeof(uint64_t));
    }
#endif

    if (args.hasSeed) {
        srand((unsigned int) args.seed);
        vm->hasFixedSeed = true;
        printf("Using fixed RNG seed: %d\n", args.seed);
    }

    if (args.printRooms) {
        // Under --lazy-rooms we load each room for display and then free it again so the dump
        // reflects what each room contains without keeping all of them resident simultaneously.
        forEachIndexed(Room, room, idx, dataWin->room.rooms, dataWin->room.count) {
            bool loadedHere = false;
            if (!room->payloadLoaded) {
                DataWin_loadRoomPayload(dataWin, (int32_t) idx);
                loadedHere = true;
            }

            printf("[%d] %s ()\n", idx, room->name);

            forEachIndexed(RoomGameObject, roomGameObject, idx2, room->gameObjects, room->gameObjectCount) {
                GameObject* gameObject = &dataWin->objt.objects[roomGameObject->objectDefinition];
                printf(
                    "  [%d] %s (x=%d,y=%d,persistent=%d,solid=%d,spriteId=%d,preCreateCode=%d,creationCode=%d)\n",
                    idx2,
                    gameObject->name,
                    roomGameObject->x,
                    roomGameObject->y,
                    gameObject->persistent,
                    gameObject->solid,
                    gameObject->spriteId,
                    roomGameObject->preCreateCode,
                    roomGameObject->creationCode
                );
            }

            if (loadedHere && !room->eagerlyLoaded) {
                DataWin_freeRoomPayload(room);
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        return 0;
    }

    if (args.printDeclaredFunctions) {
        repeat(hmlen(vm->codeIndexByName), i) {
            printf("[%d] %s\n", vm->codeIndexByName[i].value, vm->codeIndexByName[i].key);
        }
        VM_free(vm);
        DataWin_free(dataWin);
        return 0;
    }

    if (shlen(args.disassemble) > 0) {
        VM_buildCrossReferences(vm);
        if (shgeti(args.disassemble, "*") >= 0) {
            repeat(dataWin->code.count, i) {
                VM_disassemble(vm, (int32_t) i);
            }
        } else {
            for (ptrdiff_t i = 0; shlen(args.disassemble) > i; i++) {
                const char* name = args.disassemble[i].key;
                ptrdiff_t idx = shgeti(vm->codeIndexByName, (char*) name);
                if (idx >= 0) {
                    VM_disassemble(vm, vm->codeIndexByName[idx].value);
                } else {
                    fprintf(stderr, "Error: Script '%s' not found in funcMap\n", name);
                }
            }
        }
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 0;
    }

    // Initialize the file system
    char* dataWinDir = nullptr;
    {
        const char* lastSlash = strrchr(args.dataWinPath, '/');
        const char* lastBackslash = strrchr(args.dataWinPath, '\\');
        if (lastBackslash != nullptr && (lastSlash == nullptr || lastBackslash > lastSlash))
            lastSlash = lastBackslash;
        if (lastSlash != nullptr) {
            size_t len = (size_t) (lastSlash - args.dataWinPath + 1);
            dataWinDir = safeMalloc(len + 1);
            memcpy(dataWinDir, args.dataWinPath, len);
            dataWinDir[len] = '\0';
        } else {
            dataWinDir = safeStrdup("./");
        }
    }
    const char* savePath = args.saveFolder != nullptr ? args.saveFolder : dataWinDir;
    OverlayFileSystem* overlayFs = OverlayFileSystem_create(dataWinDir, savePath);
    free(dataWinDir);

    // ===[ Win32 window creation ]===
    HINSTANCE hInstance = GetModuleHandle(NULL);

    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ButterscotchD3D9WindowClass";

    if (!RegisterClassExA(&wc)) {
        fprintf(stderr, "Failed to register window class\n");
        OverlayFileSystem_destroy(overlayFs);
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (args.headless) {
        style &= ~WS_VISIBLE;
    }

    RECT rect = {0, 0, (LONG)gen8->defaultWindowWidth, (LONG)gen8->defaultWindowHeight};
    AdjustWindowRect(&rect, style, FALSE);

    g_window = CreateWindowExA(
        0,
        wc.lpszClassName,
        windowTitle,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        NULL,
        hInstance,
        NULL
    );

    if (!g_window) {
        fprintf(stderr, "Failed to create Win32 window\n");
        OverlayFileSystem_destroy(overlayFs);
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    if (!args.headless) {
        ShowWindow(g_window, SW_SHOW);
        UpdateWindow(g_window);
    }

    // ===[ D3D9 initialization ]===
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(stderr, "Direct3DCreate9 failed\n");
        DestroyWindow(g_window);
        OverlayFileSystem_destroy(overlayFs);
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = g_window;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* d3dDevice = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(
        d3d,
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        g_window,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
        &pp,
        &d3dDevice
    );

    IDirect3D9_Release(d3d);

    if (FAILED(hr) || !d3dDevice) {
        fprintf(stderr, "CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
        DestroyWindow(g_window);
        OverlayFileSystem_destroy(overlayFs);
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Initialize the renderer
    Renderer* renderer = (Renderer*)D3D9Renderer_create(d3dDevice);
    if (!renderer) {
        fprintf(stderr, "Failed to create D3D9 renderer\n");
        IDirect3DDevice9_Release(d3dDevice);
        DestroyWindow(g_window);
        OverlayFileSystem_destroy(overlayFs);
        VM_free(vm);
        DataWin_free(dataWin);
        freeCommandLineArgs(&args);
        return 1;
    }

    // Initialize the audio system
    AudioSystem* audioSystem = nullptr;
    if (args.headless) {
        audioSystem = (AudioSystem*) NoopAudioSystem_create();
    } else {
#if defined(USE_OPENAL)
        audioSystem = (AudioSystem*) AlAudioSystem_create();
#elif defined(USE_MINIAUDIO)
        audioSystem = (AudioSystem*) MaAudioSystem_create();
#else
        audioSystem = (AudioSystem*) NoopAudioSystem_create();
#endif
    }

    // Initialize the runner
    Runner* runner = Runner_create(dataWin, vm, renderer, (FileSystem*) overlayFs, audioSystem);
    runner->debugMode = args.debug;
    runner->osType = args.osType;
    runner->setWindowTitle = setWin32WindowTitle;
    runner->windowHasFocus = getWin32WindowFocus;
    runner->nativeWindow = g_window;
    g_runner = runner;

    // Set up input recording/playback (both can be active: playback then continue recording)
    if (args.playbackInputsPath != nullptr) {
        globalInputRecording = InputRecording_createPlayer(args.playbackInputsPath, args.recordInputsPath);
    } else if (args.recordInputsPath != nullptr) {
        globalInputRecording = InputRecording_createRecorder(args.recordInputsPath);
    }
    if (globalInputRecording != nullptr) {
        installCrashHandlers();
    }
    shcopyFromTo(args.varReadsToBeTraced, runner->vmContext->varReadsToBeTraced);
    shcopyFromTo(args.varWritesToBeTraced, runner->vmContext->varWritesToBeTraced);
    shcopyFromTo(args.functionCallsToBeTraced, runner->vmContext->functionCallsToBeTraced);
    shcopyFromTo(args.alarmsToBeTraced, runner->vmContext->alarmsToBeTraced);
    shcopyFromTo(args.instanceLifecyclesToBeTraced, runner->vmContext->instanceLifecyclesToBeTraced);
    shcopyFromTo(args.eventsToBeTraced, runner->vmContext->eventsToBeTraced);
    shcopyFromTo(args.collisionsToBeTraced, runner->vmContext->collisionsToBeTraced);
    shcopyFromTo(args.opcodesToBeTraced, runner->vmContext->opcodesToBeTraced);
    shcopyFromTo(args.stackToBeTraced, runner->vmContext->stackToBeTraced);
    shcopyFromTo(args.tilesToBeTraced, runner->vmContext->tilesToBeTraced);
    runner->vmContext->traceBytecodeAfterFrame = args.traceBytecodeAfterFrame;
    runner->vmContext->alwaysLogUnknownFunctions = args.alwaysLogUnknownFunctions;
    runner->vmContext->alwaysLogStubbedFunctions = args.alwaysLogStubbedFunctions;
    runner->vmContext->traceEventInherited = args.traceEventInherited;

#ifndef _WIN32
    struct sigaction sa = { .sa_handler = onCrashSignal };
    sigemptyset(&sa.sa_mask);
    struct sigaction prev;
    sigaction(SIGABRT, &sa, &prev);
    PreviousSignalActionEntry p;
    p.key = SIGABRT;
    p.value = prev;
    hmputs(previousSignalActions, p);
    sigaction(SIGSEGV, &sa, &prev);
    PreviousSignalActionEntry p2;
    p.key = SIGSEGV;
    p.value = prev;
    hmputs(previousSignalActions, p2);
#endif

    // Initialize the first room and fire Game Start / Room Start events
    Runner_initFirstRoom(runner);

    // Main loop
    bool debugPaused = false;
    bool debugShowCollisionMasks = false;
    double lastFrameTime = nowTimeSeconds();

    bool screenshotWarned = false;
    bool screenshotSurfacesWarned = false;

    while (true) {


        // Clear last frame's pressed/released state, then poll new input events
        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerGamepad_beginFrame(runner->gamepads);


        // Win32 message pump
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_windowShouldClose = true;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        bool shouldWindowClose = g_windowShouldClose;
        if (runner->shouldExit || shouldWindowClose)
            break;


        // Process input recording/playback (must happen after events, before Runner_step)
        InputRecording_processFrame(globalInputRecording, runner->keyboard, runner->frameCount);

        // Debug key bindings
        if (runner->debugMode) {
            // Pause
            if (RunnerKeyboard_checkPressed(runner->keyboard, 'P')) {
                debugPaused = !debugPaused;
                fprintf(stderr, "Debug: %s\n", debugPaused ? "Paused" : "Resumed");
            }

            // Go to next room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEUP)) {
                DataWin* dw = runner->dataWin;
                if ((int32_t) dw->gen8.roomOrderCount > runner->currentRoomOrderPosition + 1) {
                    int32_t nextIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition + 1];
                    runner->pendingRoom = nextIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to next room -> %s\n", dw->room.rooms[nextIdx].name);
                }
            }

            // Go to previous room
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_PAGEDOWN)) {
                DataWin* dw = runner->dataWin;
                if (runner->currentRoomOrderPosition > 0) {
                    int32_t prevIdx = dw->gen8.roomOrder[runner->currentRoomOrderPosition - 1];
                    runner->pendingRoom = prevIdx;
                    runner->audioSystem->vtable->stopAll(runner->audioSystem);
                    fprintf(stderr, "Debug: Going to previous room -> %s\n", dw->room.rooms[prevIdx].name);
                }
            }

            // Dump runner state to console
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F12)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                Runner_dumpState(runner);
            }

            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F11)) {
                fprintf(stderr, "Debug: Dumping runner state at frame %d\n", runner->frameCount);
                char* json = Runner_dumpStateJson(runner);

                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }

                free(json);
            }

            // Toggle the collision mask debug overlay
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F2)) {
                debugShowCollisionMasks = !debugShowCollisionMasks;
                fprintf(stderr, "Debug: Collision mask overlay %s!\n", debugShowCollisionMasks ? "enabled" : "disabled");
            }

            // Reset global interact state
            if (RunnerKeyboard_checkPressed(runner->keyboard, VK_F10)) {
                int32_t interactVarId = shget(runner->vmContext->globalVarNameMap, "interact");

                runner->vmContext->globalVars[interactVarId] = RValue_makeInt32(0);
                printf("Changed global.interact [%d] value!\n", interactVarId);
            }
        }

        // Run the game step if the game is paused
        bool shouldStep = true;
        if (runner->debugMode && debugPaused) {
            shouldStep = RunnerKeyboard_checkPressed(runner->keyboard, 'O');
            if (shouldStep) fprintf(stderr, "Debug: Frame advance (frame %d)\n", runner->frameCount);
        }

        double frameStartTime = 0;

        if (shouldStep) {
            if (args.traceFrames) {
                frameStartTime = nowTimeSeconds();
                fprintf(stderr, "Frame %d (Start)\n", runner->frameCount);
            }

            // Run one game step (Begin Step, Keyboard, Alarms, Step, End Step, room transitions)
            Runner_step(runner);

            if (args.profilerFramesBetween > 0 && runner->frameCount > 0 && runner->frameCount % args.profilerFramesBetween == 0) {
                char* profilerReport = Profiler_createReport(vm->profiler, 20, args.profilerFramesBetween);
                if (profilerReport != nullptr) {
                    fprintf(stderr, "%s\n", profilerReport);
                    free(profilerReport);
                }
                Profiler_reset(vm->profiler);
            }

            // Update audio system (gain fading, cleanup ended sounds)
            float dt = (float) (nowTimeSeconds() - lastFrameTime);
            if (0.0f > dt) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f; // cap delta to avoid huge fades on lag spikes
            runner->audioSystem->vtable->update(runner->audioSystem, dt);

            // Dump full runner state if this frame was requested
            if (hmget(args.dumpFrames, runner->frameCount)) {
                Runner_dumpState(runner);
            }

            // Dump runner state as JSON if this frame was requested
            if (hmget(args.dumpJsonFrames, runner->frameCount)) {
                char* json = Runner_dumpStateJson(runner);
                if (args.dumpJsonFilePattern != nullptr) {
                    char filename[512];
                    snprintf(filename, sizeof(filename), args.dumpJsonFilePattern, runner->frameCount);
                    FILE* f = fopen(filename, "w");
                    if (f != nullptr) {
                        fwrite(json, 1, strlen(json), f);
                        fputc('\n', f);
                        fclose(f);
                        printf("JSON dump saved: %s\n", filename);
                    } else {
                        fprintf(stderr, "Error: Could not write JSON dump to '%s'\n", filename);
                    }
                } else {
                    printf("%s\n", json);
                }
                free(json);
            }
        }

        // Query actual framebuffer size
        RECT clientRect;
        GetClientRect(g_window, &clientRect);
        int fbWidth = clientRect.right - clientRect.left;
        int fbHeight = clientRect.bottom - clientRect.top;
        if (fbWidth <= 0) fbWidth = (int)gen8->defaultWindowWidth;
        if (fbHeight <= 0) fbHeight = (int)gen8->defaultWindowHeight;

        int32_t gameW = (int32_t) gen8->defaultWindowWidth;
        int32_t gameH = (int32_t) gen8->defaultWindowHeight;

        float displayScaleX;
        float displayScaleY;

        Runner_computeViewDisplayScale(runner, gameW, gameH, &displayScaleX, &displayScaleY);

        // Clear backbuffer with room background color
        DWORD clearColor;
        if (runner->drawBackgroundColor) {
            int rInt = BGR_R(runner->backgroundColor);
            int gInt = BGR_G(runner->backgroundColor);
            int bInt = BGR_B(runner->backgroundColor);
            clearColor = D3DCOLOR_XRGB(rInt, gInt, bInt);
        } else {
            clearColor = D3DCOLOR_XRGB(0, 0, 0);
        }

        IDirect3DDevice9_Clear(d3dDevice, 0, NULL, D3DCLEAR_TARGET, clearColor, 1.0f, 0);

        renderer->vtable->beginFrame(renderer, gameW, gameH, fbWidth, fbHeight);

        Runner_drawViews(runner, gameW, gameH, displayScaleX, displayScaleY, debugShowCollisionMasks);

        renderer->vtable->endFrame(renderer);

renderer->vtable->beginGUI(renderer,
                           fbWidth, fbHeight,
                           0, 0,
                           fbWidth, fbHeight);
Runner_drawGUI(runner);

renderer->vtable->endGUI(renderer);

IDirect3DDevice9_EndScene(d3dDevice);


        // Capture screenshot if this frame matches a requested frame (stubbed)
        bool shouldScreenshot = hmget(args.screenshotFrames, runner->frameCount);
        if (shouldScreenshot && !screenshotWarned) {
            fprintf(stderr, "Warning: --screenshot/--screenshot-at-frame not implemented for D3D9 build\n");
            screenshotWarned = true;
        }

        // Dump all surfaces if this frame matches a requested frame (stubbed)
        bool shouldDumpSurfaces = hmget(args.screenshotSurfacesFrames, runner->frameCount);
        if (shouldDumpSurfaces && !screenshotSurfacesWarned) {
            fprintf(stderr, "Warning: --screenshot-surfaces/--screenshot-surfaces-at-frame not implemented for D3D9 build\n");
            screenshotSurfacesWarned = true;
        }

        if (args.exitAtFrame >= 0 && runner->frameCount >= args.exitAtFrame) {
            printf("Exiting at frame %d (--exit-at-frame)\n", runner->frameCount);
            g_windowShouldClose = true;
        }

        if (shouldStep && args.traceFrames) {
            double frameElapsedMs = (nowTimeSeconds() - frameStartTime) * 1000.0;
            fprintf(stderr, "Frame %d (End, %.2f ms)\n", runner->frameCount, frameElapsedMs);
        }

        // Only present when there isn't a room change to match the original runner.
        if (runner->pendingRoom == -1) {
            IDirect3DDevice9_Present(d3dDevice, NULL, NULL, NULL, NULL);
        }
        Runner_handlePendingRoomChange(runner);

// === FPS COUNTER ===
{
    static double fpsAccum = 0.0;
    static int fpsFrames = 0;
    static double fpsLastTime = 0.0;

    double now = nowTimeSeconds();
    double dt = now - fpsLastTime;
    fpsLastTime = now;

    fpsAccum += dt;
    fpsFrames++;

    if (fpsAccum >= 1.0) {
        double fps = fpsFrames / fpsAccum;

        wchar_t title[256];
        swprintf(title, 256, L"Butterscotch - %S  |  FPS: %.1f",
                 gen8->displayName, fps);

        SetWindowTextW(g_window, title);

        fpsAccum = 0.0;
        fpsFrames = 0;
    }
}


        // Limit frame rate to room speed (skip in headless mode for max speed!!)
        if (!args.headless && runner->currentRoom->speed > 0) {
            static bool fastForwardActive = false;
            static bool fastForwardTabPrev = false;
            bool fastForwardTabNow = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
            if (args.fastForwardSpeed > 0.0 && fastForwardTabNow && !fastForwardTabPrev) {
                fastForwardActive = !fastForwardActive;
                lastFrameTime = nowTimeSeconds();
            }
            fastForwardTabPrev = fastForwardTabNow;
            double effectiveSpeed = (args.fastForwardSpeed > 0.0 && fastForwardActive) ? args.fastForwardSpeed : args.speedMultiplier;
            double targetFrameTime = 1.0 / (runner->currentRoom->speed * effectiveSpeed);
            double nextFrameTime = lastFrameTime + targetFrameTime;
            double remaining = nextFrameTime - nowTimeSeconds();
            if (remaining > 0.002) {
                Sleep((DWORD)((remaining - 0.001) * 1000));
            }
            while (nowTimeSeconds() < nextFrameTime) {
                // spin-wait
            }
            lastFrameTime = nextFrameTime;
        } else {
            lastFrameTime = nowTimeSeconds();
        }
    }

    saveInputRecording();

    // Cleanup
    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    renderer->vtable->destroy(renderer);

    IDirect3DDevice9_Release(d3dDevice);
    DestroyWindow(g_window);

    Runner_free(runner);
    OverlayFileSystem_destroy(overlayFs);
#ifdef ENABLE_VM_OPCODE_PROFILER
    VM_printOpcodeProfilerReport(vm);
#endif
    VM_free(vm);
    DataWin_free(dataWin);

    freeCommandLineArgs(&args);

    printf("Bye! :3\n");
    return 0;
}
