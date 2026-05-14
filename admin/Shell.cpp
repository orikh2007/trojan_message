#include "Shell.h"

Shell::Shell() : out_r_(), in_w_(), pi_() {}
void Shell::start_shell()
{
    HANDLE in_r, out_w;
    SECURITY_ATTRIBUTES sa {
        sizeof(SECURITY_ATTRIBUTES),
        nullptr,
        TRUE};
    CreatePipe(&in_r, &in_w_, &sa, 0);
    CreatePipe(&out_r_, &out_w, &sa, 0);
    SetHandleInformation(in_w_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r_, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si {
    .cb = sizeof(STARTUPINFOA),
    .hStdInput = in_r,
    .hStdOutput = out_w,
    .hStdError = out_w,};
    si.dwFlags = STARTF_USESTDHANDLES;

    char cmd[] = "cmd.exe /Q /D";
    CreateProcessA(nullptr, cmd, nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi_);
    CloseHandle(in_r);
    CloseHandle(out_w);

    // drain banner with a sleep+peek (no sentinel yet)
    Sleep(150);
    DWORD avail = 0;
    while (PeekNamedPipe(out_r_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        char tmp[512]; DWORD n;
        ReadFile(out_r_, tmp, min(avail, (DWORD)sizeof(tmp)), &n, nullptr);
    }

    // set prompt = sentinel, drain its own output
    const char* setup = "PROMPT __DONE__$_\n";
    DWORD n;
    WriteFile(in_w_, setup, strlen(setup), &n, nullptr);

    std::string out; char buf[512];
    while (out.find("__DONE__") == std::string::npos) {
        ReadFile(out_r_, buf, sizeof(buf), &n, nullptr);
        out.append(buf, n);
    }
}

std::string Shell::run(const std::string& cmd) {
    std::string line = cmd + "\n";   // no echo __DONE__ needed anymore
    DWORD n;
    WriteFile(in_w_, line.c_str(), line.size(), &n, nullptr);

    std::string out; char buf[512];
    while (out.find("__DONE__") == std::string::npos) {
        ReadFile(out_r_, buf, sizeof(buf), &n, nullptr);
        out.append(buf, n);
    }

    // strip __DONE__ line and everything after
    auto pos = out.find("__DONE__");
    if (pos != std::string::npos) {
        auto line_start = out.rfind('\n', pos);
        out.erase(line_start == std::string::npos ? 0 : line_start);
    }
    return out;
}

void Shell::stop_shell()
{
    CloseHandle(in_w_);
    WaitForSingleObject(pi_.hProcess, 3000);
    CloseHandle(pi_.hProcess);  // release process handle
    CloseHandle(pi_.hThread);   // release thread handle
    CloseHandle(out_r_);     // release our read end of stdout
}



