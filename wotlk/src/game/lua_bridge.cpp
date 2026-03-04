#include "lua_bridge.h"
#include "../hooks/hooks.h"
#include "../offsets/offsets.h"
#include "mem.h"

#define NOMINMAX
#include <Windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <glog/logging.h>

namespace game::lua {

// FrameScript_GetText: const char* __cdecl(const char* varName, int unk, int unk2)
using GetTextFn = const char*(__cdecl*)(const char*, int, int);
static constexpr uintptr_t kGetTextAddr = offsets::fn::FrameScript_GetText;

// SEH-safe wrapper — no C++ objects on stack
static const char* __cdecl CallGetText(const char* varName)
{
    auto getText = reinterpret_cast<GetTextFn>(kGetTextAddr);
    __try {
        return getText(varName, -1, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool Execute(const char* code)
{
    auto fn = hooks::GetOriginalFrameScriptExecute();
    if (!fn || !code) return false;
    fn(code, "game_sdk", 0);
    return true;
}

bool Executef(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return Execute(buf);
}

std::string GetValue(const char* expr)
{
    if (!expr || !expr[0]) return {};

    char buf[2048];
    snprintf(buf, sizeof(buf), "_wt=tostring(%s)", expr);
    if (!Execute(buf))
        return {};

    const char* result = CallGetText("_wt");
    if (!result) return {};
    return std::string(result);
}

int GetInt(const char* expr)
{
    std::string val = GetValue(expr);
    if (val.empty()) return 0;
    return std::atoi(val.c_str());
}

float GetFloat(const char* expr)
{
    std::string val = GetValue(expr);
    if (val.empty()) return 0.f;
    return static_cast<float>(std::atof(val.c_str()));
}

bool GetBool(const char* expr)
{
    std::string val = GetValue(expr);
    return !val.empty() && val != "nil" && val != "0" && val != "false";
}

void SetupErrorCapture()
{
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    // Chain before the existing error handler so WoW's UI still shows errors.
    Execute(
        "_dlua_prev_err = geterrorhandler();"
        "seterrorhandler(function(e)"
        "  _dlua_err = (_dlua_err or '') .. tostring(e) .. '\\n';"
        "  if _dlua_prev_err then _dlua_prev_err(e) end;"
        "end);"
    );
}


void FlushCapturedErrors()
{
    const char* raw = CallGetText("_dlua_err");
    if (!raw || raw[0] == '\0') return;

    std::string errors(raw);
    if (errors == "nil") return;

    LOG(WARNING) << "[LUA] Captured Lua error(s):\n" << errors;
    Execute("_dlua_err = nil");
}

} // namespace game::lua
