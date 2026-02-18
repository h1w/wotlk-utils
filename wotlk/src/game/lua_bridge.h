#pragma once
// =============================================================================
// Lua bridge -- execute Lua code and retrieve values via FrameScript.
//
// Uses the ORIGINAL FrameScript_Execute trampoline (bypasses our logging hook)
// and FrameScript_GetText to extract return values.
// =============================================================================

#include <string>

namespace game::lua {

// Execute arbitrary Lua code. Returns true if the function pointer is valid.
bool Execute(const char* code);

// printf-style execute (formats into a stack buffer).
bool Executef(const char* fmt, ...);

// Execute a Lua expression and return its string result.
// Internally does: Execute("_wt=tostring(<expr>)") then reads _wt via GetText.
std::string GetValue(const char* expr);

// Convenience wrappers
int         GetInt(const char* expr);
float       GetFloat(const char* expr);
bool        GetBool(const char* expr);

} // namespace game::lua
