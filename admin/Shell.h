#pragma once
#include <iostream>
#include <string>
#include <windows.h>

class Shell
{
public:
    Shell();
    void start_shell();
    std::string run(const std::string& cmd);
    void stop_shell();
private:
    HANDLE out_r_, in_w_;
    PROCESS_INFORMATION pi_;
};


