#include "stdafx.h"
#include "helper.hpp"

#include <inipp/inipp.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <safetyhook.hpp>

HMODULE baseModule = GetModuleHandle(NULL);

// Version
std::string sFixName = "WukongTweak";
std::string sFixVer = "0.8.1";
std::string sLogFile = sFixName + ".log";

// Logger
std::shared_ptr<spdlog::logger> logger;
std::filesystem::path sExePath;
std::string sExeName;

// Ini
inipp::Ini<char> ini;
std::string sConfigFile = sFixName + ".ini";
std::pair DesktopDimensions = { 0,0 };

// Ini variables
bool bFixAspectLimit;
float fSharpeningValue;
bool bChromaticAberration;
bool bVignette;
bool bRayReconstruction;
float fAdditionalFOV;

// Variables
bool bCachedConsoleObjects = false;
IConsoleVariable* cvarSharpen;
IConsoleVariable* cvarCA;
IConsoleVariable* cvarVignette;
IConsoleVariable* cvarRR;

// CVAR addresses
UC::TMap<UC::FString, Unreal::FConsoleObject*> ConsoleObjects;

void Logging()
{
    // Get game name and exe path
    WCHAR exePath[_MAX_PATH] = { 0 };
    GetModuleFileNameW(baseModule, exePath, MAX_PATH);
    sExePath = exePath;
    sExeName = sExePath.filename().string();
    sExePath = sExePath.remove_filename();

    // spdlog initialisation
    {
        try {
            logger = spdlog::basic_logger_st(sFixName.c_str(), sExePath.string() + sLogFile, true);
            spdlog::set_default_logger(logger);

            spdlog::flush_on(spdlog::level::debug);
            spdlog::info("----------");
            spdlog::info("{} v{} loaded.", sFixName.c_str(), sFixVer.c_str());
            spdlog::info("----------");
            spdlog::info("Path to logfile: {}", sExePath.string() + sLogFile);
            spdlog::info("----------");

            // Log module details
            spdlog::info("Module Name: {0:s}", sExeName.c_str());
            spdlog::info("Module Path: {0:s}", sExePath.string());
            spdlog::info("Module Address: 0x{0:x}", (uintptr_t)baseModule);
            spdlog::info("Module Timestamp: {0:d}", Memory::ModuleTimestamp(baseModule));
            spdlog::info("----------");
        }
        catch (const spdlog::spdlog_ex& ex) {
            AllocConsole();
            FILE* dummy;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
            std::cout << "Log initialisation failed: " << ex.what() << std::endl;
            FreeLibraryAndExitThread(baseModule, 1);
        }
    }
}

void Configuration()
{
    // Initialise config
    std::ifstream iniFile(sExePath.string() + sConfigFile);
    if (!iniFile) {
        AllocConsole();
        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        std::cout << "" << sFixName.c_str() << " v" << sFixVer.c_str() << " loaded." << std::endl;
        std::cout << "ERROR: Could not locate config file." << std::endl;
        std::cout << "ERROR: Make sure " << sConfigFile.c_str() << " is located in " << sExePath.string().c_str() << std::endl;
        FreeLibraryAndExitThread(baseModule, 1);
    }
    else {
        spdlog::info("Path to config file: {}", sExePath.string() + sConfigFile);
        ini.parse(iniFile);
    }

    // Parse config
    ini.strip_trailing_comments();
    inipp::get_value(ini.sections["Remove Aspect Ratio Limit"], "Enabled", bFixAspectLimit);
    inipp::get_value(ini.sections["Gameplay FOV"], "AdditionalFOV", fAdditionalFOV);
    inipp::get_value(ini.sections["Adjust Sharpening"], "Strength", fSharpeningValue);
    inipp::get_value(ini.sections["Chromatic Aberration"], "Enabled", bChromaticAberration);
    inipp::get_value(ini.sections["Vignette"], "Enabled", bVignette);
    inipp::get_value(ini.sections["RayReconstruction"], "Enabled", bRayReconstruction);

    spdlog::info("----------");
    spdlog::info("Config Parse: bFixAspectLimit: {}", bFixAspectLimit);
    if (fAdditionalFOV < (float)-80 || fAdditionalFOV >(float)80) {
        fAdditionalFOV = std::clamp(fAdditionalFOV, (float)-80, (float)80);
        spdlog::warn("Config Parse: fAdditionalFOV value invalid, clamped to {}", fAdditionalFOV);
    }
    spdlog::info("Config Parse: fAdditionalFOV: {}", fAdditionalFOV);
    spdlog::info("Config Parse: fSharpeningValue: {}", fSharpeningValue);
    spdlog::info("Config Parse: bChromaticAberration: {}", bChromaticAberration);
    spdlog::info("Config Parse: bVignette: {}", bVignette);
    spdlog::info("Config Parse: bRayReconstruction: {}", bRayReconstruction);
    spdlog::info("----------");

    // Grab desktop resolution/aspect
    DesktopDimensions = Util::GetPhysicalDesktopDimensions();
}


void GetCVARs()
{
    // Get console objects
    uint8_t* ConsoleManagerSingletonScanResult = Memory::PatternScan(baseModule, "48 83 ?? ?? 48 83 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? B9 ?? ?? ?? ?? 48 89 ?? ?? ?? E8 ?? ?? ?? ?? 48 ?? ?? 48 ?? ??");
    if (ConsoleManagerSingletonScanResult) {
        spdlog::info("Console CVARs: Address is {:s}+{:x}", sExeName.c_str(), (uintptr_t)ConsoleManagerSingletonScanResult - (uintptr_t)baseModule);
        uintptr_t singletonAddr = Memory::GetAbsolute((uintptr_t)ConsoleManagerSingletonScanResult + 0x7) + 0x1;
        spdlog::info("Console CVARs: IConsoleManager singleton address is {:s}+{:x}", sExeName.c_str(), (uintptr_t)singletonAddr - (uintptr_t)baseModule);

        int i = 0;
        while (!*(uintptr_t*)singletonAddr)
        {
            i++;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            if (i == 100) { // 10s
                spdlog::error("Console CVARs: Failed to find singleton adddress.");
                return;
            }
        }

        // Cache all console objects
        ConsoleObjects = Unreal::GetConsoleObjects(singletonAddr);

        if (ConsoleObjects.Num() != 0) {
            bCachedConsoleObjects = true;
            spdlog::info("Console CVARs: Cached all console objects.");
        }
        else {
            spdlog::error("Console CVARs: ConsoleObjects TMap is not valid.");
        }
    }
    else if (!ConsoleManagerSingletonScanResult) {
        spdlog::error("Console CVARs: Pattern scan failed.");
    }

    if (bCachedConsoleObjects) {
        // r.Tonemapper.Sharpen
        cvarSharpen = Unreal::FindCVAR("r.Tonemapper.Sharpen", ConsoleObjects);
        if (cvarSharpen) {
            spdlog::info("CVar: r.Tonemapper.Sharpen: Address {:x}", (uintptr_t)cvarSharpen);
        }

        // r.SceneColorFringeQuality
        cvarCA = Unreal::FindCVAR("r.SceneColorFringeQuality", ConsoleObjects);
        if (cvarCA) {
            spdlog::info("CVar: r.SceneColorFringeQuality: Address {:x}", (uintptr_t)cvarCA);
        }

        // r.Tonemapper.Quality
        cvarVignette = Unreal::FindCVAR("r.Tonemapper.Quality", ConsoleObjects);
        if (cvarVignette) {
            spdlog::info("CVar: r.Tonemapper.Quality: Address {:x}", (uintptr_t)cvarVignette);
        }

        cvarRR = Unreal::FindCVAR("r.NGX.DLSS.DenoiserMode", ConsoleObjects);
        if (cvarRR) {
			spdlog::info("CVar: r.NGX.DLSS.DenoiserMode: Address {:x}", (uintptr_t)cvarRR);
		}
    }
}

void SetCVARs()
{
    // ULevelSequence::PostLoad
    // Set CVars after the level has loaded so that we override anything set by the game.
    uint8_t* LevelPostLoadScanResult = Memory::PatternScan(baseModule, "40 ?? 48 ?? ?? ?? 48 ?? ?? E8 ?? ?? ?? ?? 80 ?? ?? ?? ?? ?? 03 75 ?? 0F ?? ?? ?? ?? ?? ?? 48 ?? ?? C0 ?? 02");
    if (LevelPostLoadScanResult) {
        spdlog::info("LevelSequence:PostLoad: Address is {:s}+{:x}", sExeName.c_str(), (uintptr_t)LevelPostLoadScanResult - (uintptr_t)baseModule);
        static SafetyHookMid LevelPostLoadMidHook{};
        LevelPostLoadMidHook = safetyhook::create_mid(LevelPostLoadScanResult,
            [](SafetyHookContext& ctx) {
                // r.Tonemapper.Sharpen
                if (cvarSharpen && (cvarSharpen->GetFloat() != fSharpeningValue)) {
                    // Flag jank
                    *reinterpret_cast<int*>((uintptr_t)cvarSharpen + 0x18) = 0x0A000000;
                    // Set value manually since this one is finicky
                    *reinterpret_cast<float*>((uintptr_t)cvarSharpen + 0x60) = fSharpeningValue;
                    *reinterpret_cast<float*>((uintptr_t)cvarSharpen + 0x64) = fSharpeningValue;
                    spdlog::info("CVar: r.Tonemapper.Sharpen: Set to {}", cvarSharpen->GetFloat());
                }

                // r.SceneColorFringeQuality
                if (cvarCA && (cvarCA->GetInt() != (int)bChromaticAberration)) {
                    cvarCA->Set(std::to_wstring((int)bChromaticAberration).c_str());
                    spdlog::info("CVar: r.SceneColorFringeQuality: Set to {}", cvarCA->GetInt());
                }    

                // r.Tonemapper.Quality
                if (cvarVignette && (cvarVignette->GetInt() != 1) && !bVignette) {
                    cvarVignette->Set(L"1");
                    spdlog::info("CVar: r.Tonemapper.Quality: Set to {}", cvarVignette->GetInt());
                }

                // r.NGX.DLSS.DenoiserMode
                if (cvarRR && (cvarRR->GetInt() != (int)bRayReconstruction)) {
                    cvarRR->Set(std::to_wstring((int)bRayReconstruction).c_str());
                    spdlog::info("CVar: r.NGX.DLSS.DenoiserMode: Set to {}", cvarRR->GetInt());
                }
            });
    }
    else if (!LevelPostLoadScanResult) {
        spdlog::error("LevelSequence:PostLoad: Pattern scan failed.");
    }
}

void Tweaks()
{
    // Remove aspect ratio limit
    if (bFixAspectLimit) {
        uint8_t* AspectRatioLimitScanResult = Memory::PatternScan(baseModule, "45 ?? ?? 74 ?? 0F ?? ?? 0F ?? ?? FE ?? 41 ?? ?? C3");
        if (AspectRatioLimitScanResult) {
            spdlog::info("Aspect Ratio Limit: Address is {:s}+{:x}", sExeName.c_str(), (uintptr_t)AspectRatioLimitScanResult - (uintptr_t)baseModule);
            Memory::PatchBytes((uintptr_t)AspectRatioLimitScanResult + 0x3, "\x90\x90", 2);
            spdlog::info("Aspect Ratio Limit: Patched instruction.");
        }
        else if (!AspectRatioLimitScanResult) {
            spdlog::error("Aspect Ratio Limit: Pattern scan failed.");
        }
    }

    // Gameplay FOV
    if (fAdditionalFOV != 0.00f) {
        uint8_t* GameplayFOVScanResult = Memory::PatternScan(baseModule, "F3 0F ?? ?? ?? 03 ?? ?? F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? ?? ?? ?? ?? 48 8B ?? ?? 48 8B ?? 83 ?? 00 F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? ?? ?? ?? ?? F3 0F ?? ?? 48 8B ?? ?? 48 8B ?? 83 ?? 00"); // stupidly long pattern (:
        if (GameplayFOVScanResult) {
            spdlog::info("Gameplay FOV: Address is {:s}+{:x}", sExeName.c_str(), (uintptr_t)GameplayFOVScanResult - (uintptr_t)baseModule);
            static SafetyHookMid GameplayFOVMidHook{};
            GameplayFOVMidHook = safetyhook::create_mid(GameplayFOVScanResult + 0x8,
                [](SafetyHookContext& ctx) {
                    ctx.xmm0.f32[0] += fAdditionalFOV;
                });
        }
        else if (!GameplayFOVScanResult) {
            spdlog::error("Gameplay FOV: Pattern scan failed.");
        }
    }
}

DWORD __stdcall Main(void*)
{
    Logging();
    Configuration();
    GetCVARs();
    SetCVARs();
    Tweaks();
    return true;
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
    )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        HANDLE mainHandle = CreateThread(NULL, 0, Main, 0, NULL, 0);
        if (mainHandle)
        {
            CloseHandle(mainHandle);
        }
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}