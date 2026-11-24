#pragma once
#include <string>

namespace nl {

// The folder that holds ninelink.dll (and log\ beside it).
const std::wstring& mod_dir();
void set_mod_dir(const std::wstring& dir);

// Opens log\ninelink_<time>.log. Safe to call once.
void log_open();
void logf(const char* fmt, ...);

std::string narrow(const std::wstring& w);

} // namespace nl
