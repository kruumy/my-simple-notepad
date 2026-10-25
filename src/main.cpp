#include "app.h"
#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring path = (argv != nullptr && argc > 1) ? argv[1] : L"";
    LocalFree(argv);
    return simple_notepad::app().run(path);
}
