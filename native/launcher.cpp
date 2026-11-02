#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

// The shortcut must start a GUI executable. Hiding PowerShell after it starts
// still allows Windows to create and briefly display its console first.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::vector<wchar_t> module(32768);
    DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return ERROR_BAD_PATHNAME;
    const auto root = std::filesystem::path(std::wstring(module.data(), length)).parent_path();
    const auto script = root / L"Open-Codex.ps1";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"Open-Codex.ps1 is missing. Extract or install the complete Session Stats package.", L"ChatGPT launcher", MB_OK | MB_ICONERROR);
        return ERROR_FILE_NOT_FOUND;
    }
    wchar_t system[MAX_PATH];
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    if (!systemLength || systemLength >= MAX_PATH) return ERROR_BAD_PATHNAME;
    const auto powershell = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command = L"\"" + powershell.wstring() + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &child)) {
        const DWORD error = GetLastError();
        MessageBoxW(nullptr, L"Could not start Windows PowerShell for ChatGPT.", L"ChatGPT launcher", MB_OK | MB_ICONERROR);
        return static_cast<int>(error);
    }
    CloseHandle(child.hThread);
    const DWORD wait = WaitForSingleObject(child.hProcess, INFINITE);
    DWORD result = ERROR_GEN_FAILURE;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess);
    return static_cast<int>(result);
}
