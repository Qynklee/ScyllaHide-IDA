#define USE_STANDARD_FILE_FUNCTIONS
#pragma warning(disable : 4996 4512 4127 4201)


//for 64bit - p64
#ifdef BUILD_IDA_64BIT
#define __EA64__
#pragma comment(lib, "x64_win_vc_64/ida.lib")
#else
//for 32bit - plw
#pragma comment(lib, "x64_win_vc_32/ida.lib")
#endif

#include <Windows.h>
#include <ida.hpp>
#include <idp.hpp>
#include <dbg.hpp>
#include <loader.hpp>
#include <kernwin.hpp>
#include <Scylla/Logger.h>
#include <Scylla/Settings.h>
#include <Scylla/Version.h>
#include <Scylla/Util.h>
#include <Scylla/OsInfo.h>

#include "..\PluginGeneric\Injector.h"
#include "..\PluginGeneric\OptionsDialog.h"
#include "IdaServerClient.h"
#include "resource.h"

typedef void(__cdecl * t_AttachProcess)(DWORD dwPID);

extern t_AttachProcess _AttachProcess;

const WCHAR g_scyllaHideDllFilename32[] = L"HookLibraryx86.dll";
const WCHAR g_scyllaHideDllFilename64[] = L"HookLibraryx64.dll";
const WCHAR g_scyllaHidex86ServerFilename[] = L"ScyllaHideIDAServerx86.exe";
const WCHAR g_scyllaHidex64ServerFilename[] = L"ScyllaHideIDAServerx64.exe";

scl::Settings g_settings;
scl::Logger g_log;
std::wstring g_scyllaHideDllPath;
std::wstring g_scyllaHideIniPath;
std::wstring g_scyllaHideServerPath;

HOOK_DLL_DATA g_hdd;

//globals
HINSTANCE hinst;
DWORD ProcessId = 0;
bool bHooked = false;
HMODULE hNtdllModule = 0;
PROCESS_INFORMATION ServerProcessInfo = { 0 };
STARTUPINFO ServerStartupInfo = { 0 };
bool isAttach = false;


static void LogCallback(const char *message)
{
    msg("[%s] %s\n", SCYLLA_HIDE_NAME_A, message);
}

static void AttachProcess(DWORD dwPID)
{
    int res = attach_process((pid_t)dwPID);

    switch (res) {
    case -1:
    {
        MessageBoxA(GetForegroundWindow(),
            "Can't attach to that process !",
            "ScyllaHide Plugin", MB_OK | MB_ICONERROR);
        break;
    }
    case -2:
    {
        MessageBoxA(GetForegroundWindow(),
            "Can't find that PID !",
            "ScyllaHide Plugin", MB_OK | MB_ICONERROR);
        break;
    }
    }
}

static bool SetDebugPrivileges()
{
    TOKEN_PRIVILEGES Debug_Privileges;
    bool retVal = false;

    if (LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Debug_Privileges.Privileges[0].Luid))
    {
        HANDLE hToken = 0;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken))
        {
            Debug_Privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            Debug_Privileges.PrivilegeCount = 1;

            retVal = AdjustTokenPrivileges(hToken, FALSE, &Debug_Privileges, 0, NULL, NULL) != FALSE;

            CloseHandle(hToken);
        }
    }

    return retVal;
}

//callback for various debug events
static ssize_t idaapi debug_mainloop(void *user_data, int notif_code, va_list va)
{
    switch (notif_code)
    {
    case dbg_process_attach:
    {
        isAttach = true;
        break; //attaching not supported
    }
    case dbg_process_start:
    {
        isAttach = false;

        const debug_event_t* dbgEvent = va_arg(va, const debug_event_t*);

        ProcessId = dbgEvent->pid;
        bHooked = false;
        ZeroMemory(&g_hdd, sizeof(HOOK_DLL_DATA));

        if (dbg != nullptr)
        {
            //char text[1000];
            //wsprintfA(text, "dbg->id %d processor %s", dbg->id , dbg->processor);
            //MessageBoxA(0, text, text,0);
            // dbg->id DEBUGGER_ID_WINDBG -> 64bit and 32bit
            // dbg->id DEBUGGER_ID_X86_IA32_WIN32_USER -> 32bit

            if (dbg->is_remote())
            {
            do_remote:
                g_log.LogInfo(L"Remote Debug Detected !\n");

                qstring hoststring;
                char host[MAX_PATH] = { 0 };
                char port[6] = { 0 };
                wcstombs(port, g_settings.opts().idaServerPort.c_str(), _countof(port));

                get_process_options(NULL, NULL, NULL, &hoststring, NULL, NULL);
                GetHost((char*)hoststring.c_str(), host);

                g_log.LogInfo(L"HOST: ", hoststring.c_str());

                //msg("Host-String: %s\n", hoststring.c_str());
                //msg("Host: %s\n", host);

                //autostart server if necessary
                if(g_settings.opts().idaAutoStartServer)
                {
                    if (!scl::FileExistsW(g_scyllaHideServerPath.c_str()))
                    {
                        g_log.LogError(L"Cannot find server executable %s\n", g_scyllaHideServerPath.c_str());
                    }

                    DWORD dwRunningStatus = 0;
                    if (ServerProcessInfo.hProcess)
                    {
                        GetExitCodeProcess(ServerProcessInfo.hProcess, &dwRunningStatus);
                    }

                    if(dwRunningStatus != STILL_ACTIVE)
                    {
                        if (ServerProcessInfo.hProcess)
                        {
                            CloseHandle(ServerProcessInfo.hProcess);
                            CloseHandle(ServerProcessInfo.hThread);
                        }

                        ZeroMemory(&ServerStartupInfo, sizeof(ServerStartupInfo));
                        ZeroMemory(&ServerProcessInfo, sizeof(ServerProcessInfo));

                        WCHAR commandline[MAX_PATH*2] = {0};
                        wcscpy(commandline, g_scyllaHideServerPath.c_str());
                        wcscat(commandline, L" ");
                        wcscat(commandline, g_settings.opts().idaServerPort.c_str());
                        ServerStartupInfo.cb = sizeof(ServerStartupInfo);
                        if (!CreateProcessW(0, commandline, NULL, NULL, FALSE, 0, NULL, NULL, &ServerStartupInfo, &ServerProcessInfo))
                        {
                            g_log.LogError(L"Cannot start server, error %d", GetLastError());
                        }
                        else
                        {
                            g_log.LogInfo(L"Started IDA Server successfully");
                        }
                    }
                }

                if (ConnectToServer(host, port))
                {
                    if (!SendEventToServer(notif_code, ProcessId))
                    {
                        g_log.LogError(L"SendEventToServer failed");
                    }
                }
                else
                {
                    g_log.LogError(L"Cannot connect to host %s", host);
                }
            }
            else
            { 

#ifndef BUILD_IDA_64BIT
                goto do_remote;
#else
                if (scl::IsWindows64() && !bHooked) // for x64 OS
                {
                    ReadNtApiInformation(&g_hdd);

                    bHooked = true;
                    startInjection(ProcessId, &g_hdd, g_scyllaHideDllPath.c_str(), true);
                }
#endif
            }
        }
    }
    break;

    case dbg_process_exit:
    {
#ifndef BUILD_IDA_64BIT
        if(!isAttach)
        {
#else

        if (!isAttach && dbg->is_remote())
        {
#endif
            if (!SendEventToServer(notif_code, ProcessId))
            {
                g_log.LogError(L"SendEventToServer failed");
            }

            CloseServerSocket();
        }
        ProcessId = 0;
        bHooked = false;
    }
    break;

    case dbg_library_load:
    {

        if (!isAttach && dbg->is_remote())
        {
        do_remote_libload:
            if (!SendEventToServer(notif_code, ProcessId))
            {
                g_log.LogError(L"SendEventToServer failed");
            }
        }
        else if (!isAttach)
        {
#ifndef BUILD_IDA_64BIT
            goto do_remote_libload;
#endif // !BUILD_IDA_64BIT

            if (bHooked)
            {
                startInjection(ProcessId, &g_hdd, g_scyllaHideDllPath.c_str(), false);
            }
        }

    }
    break;

#ifdef BUILD_IDA_64BIT
    case dbg_library_unload:
    {
        if (scl::IsWindows64() && !bHooked)
        {
            // Bogus unload event which is actually a load of a native x64 DLL (ntdll, wow64, wow64cpu, wow64win)
            ReadNtApiInformation(&g_hdd);

            bHooked = true;
            startInjection(ProcessId, &g_hdd, g_scyllaHideDllPath.c_str(), true);
        }
    }
    break;
#endif

    case dbg_bpt:
    {
        thid_t tid = va_arg(va, thid_t);
        ea_t breakpoint_ea = va_arg(va, ea_t);
        va_arg(va, int*);
    }
    break;

    case dbg_exception:
    {
        const debug_event_t* dbgEvent = va_arg(va, const debug_event_t*);

    }
    break;
    }

    return 0;
}

//cleanup on plugin unload
static void idaapi IDAP_term(void)
{
    unhook_from_notification_point(HT_DBG, debug_mainloop, NULL);
}

//called when user clicks in plugin menu or presses hotkey
static bool idaapi IDAP_run(size_t arg)
{
    DialogBoxW(hinst, MAKEINTRESOURCE(IDD_OPTIONS), GetForegroundWindow(), &OptionsDlgProc);
    return true;
}

//init the plugin
static plugmod_t *idaapi IDAP_init(void)
{
    //ensure target is PE executable
    if (inf.filetype != f_PE) return PLUGIN_SKIP;

    //install hook for debug mainloop
    if (!hook_to_notification_point(HT_DBG, debug_mainloop, NULL))
    {
        g_log.LogError(L"Error hooking notification point");
        return PLUGIN_SKIP;
    }

    msg("##################################################\n");
    msg("# " SCYLLA_HIDE_NAME_A " v" SCYLLA_HIDE_VERSION_STRING_A " Copyright 2014-" COMPILE_YEAR_A " Aguila / cypher #\n");
    msg("##################################################\n");

    bHooked = false;
    ProcessId = 0;
    ZeroMemory(&ServerStartupInfo, sizeof(ServerStartupInfo));
    ZeroMemory(&ServerProcessInfo, sizeof(ServerProcessInfo));

    return PLUGIN_KEEP;
}

// There isn't much use for these yet, but I set them anyway.
static char IDAP_comment[] = SCYLLA_HIDE_NAME_A " usermode Anti-Anti-Debug Plugin";
static char IDAP_help[] = SCYLLA_HIDE_NAME_A;

// The name of the plug-in displayed in the Edit->Plugins menu
static char IDAP_name[] = SCYLLA_HIDE_NAME_A;

// The hot-key the user can use to run your plug-in.
static char IDAP_hotkey[] = "Alt-X";

// The all-important exported PLUGIN object
idaman ida_module_data plugin_t PLUGIN =
{
    IDP_INTERFACE_VERSION,
    0,
    IDAP_init,
    IDAP_term,
    IDAP_run,
    IDAP_comment,
    IDAP_help,
    IDAP_name,
    IDAP_hotkey
};

BOOL WINAPI DllMain(HINSTANCE hInstDll, DWORD dwReason, LPVOID lpReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        hinst = hInstDll;
        _AttachProcess = AttachProcess;
        hNtdllModule = GetModuleHandleW(L"ntdll.dll");

        auto wstrPath = scl::GetModuleFileNameW(hInstDll);
        wstrPath.resize(wstrPath.find_last_of(L'\\') + 1);

#ifdef BUILD_IDA_64BIT
        g_scyllaHideDllPath = wstrPath + g_scyllaHideDllFilename64;
        g_scyllaHideServerPath = wstrPath + g_scyllaHidex64ServerFilename;
#else // !BUILD_IDA_64BIT

        g_scyllaHideDllPath = wstrPath + g_scyllaHideDllFilename32;
        g_scyllaHideServerPath = wstrPath + g_scyllaHidex86ServerFilename;
#endif

        g_scyllaHideIniPath = wstrPath + scl::Settings::kFileName;

        auto log_file = wstrPath + scl::Logger::kFileName;
        g_log.SetLogFile(log_file.c_str());
        g_log.SetLogCb(scl::Logger::Info, LogCallback);
        g_log.SetLogCb(scl::Logger::Error, LogCallback);

        g_settings.Load(g_scyllaHideIniPath.c_str());

        if (!SetDebugPrivileges())
        {
            g_log.LogInfo(L"Failed to set debug privileges");
        }

        if (!StartWinsock())
        {
            MessageBoxW(0, L"Failed to start Winsock!", L"Error", MB_ICONERROR);
        }
    }

    return TRUE;
}
