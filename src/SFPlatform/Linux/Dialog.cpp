#include "include/Dialog.h"

#include "include/Log.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace SFPlatform::Dialog {
namespace {

bool CommandAvailable(const char* cmd) {
    std::string check = "command -v " + std::string(cmd) + " >/dev/null 2>&1";
    return system(check.c_str()) == 0;
}

std::string ShellEscape(const std::string& str) {
    std::string out = "'";
    for (char c : str) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

void ShowWarning(std::string title, std::string message) {
    SFP_LOG_WARN("[Dialog] {}: {}", title, message);
    if (CommandAvailable("zenity")) {
        std::string cmd = "zenity --warning --title=" + ShellEscape(title) +
                          " --text=" + ShellEscape(message) + " 2>/dev/null &";
        system(cmd.c_str());
    } else if (CommandAvailable("kdialog")) {
        std::string cmd = "kdialog --title " + ShellEscape(title) +
                          " --sorry " + ShellEscape(message) + " 2>/dev/null &";
        system(cmd.c_str());
    }
}

void ShowInfo(std::string title, std::string message) {
    SFP_LOG_INFO("[Dialog] {}: {}", title, message);
    if (CommandAvailable("zenity")) {
        std::string cmd = "zenity --info --title=" + ShellEscape(title) +
                          " --text=" + ShellEscape(message) + " 2>/dev/null &";
        system(cmd.c_str());
    } else if (CommandAvailable("kdialog")) {
        std::string cmd = "kdialog --title " + ShellEscape(title) +
                          " --msgbox " + ShellEscape(message) + " 2>/dev/null &";
        system(cmd.c_str());
    }
}

bool ShowConfirm(std::string title, std::string message) {
    SFP_LOG_INFO("[Dialog Confirm] {}: {}", title, message);
    if (CommandAvailable("zenity")) {
        std::string cmd = "zenity --question --title=" + ShellEscape(title) +
                          " --text=" + ShellEscape(message) + " 2>/dev/null";
        return system(cmd.c_str()) == 0;
    } else if (CommandAvailable("kdialog")) {
        std::string cmd = "kdialog --title " + ShellEscape(title) +
                          " --yesno " + ShellEscape(message) + " 2>/dev/null";
        return system(cmd.c_str()) == 0;
    }
    return false;
}

} // namespace SFPlatform::Dialog
