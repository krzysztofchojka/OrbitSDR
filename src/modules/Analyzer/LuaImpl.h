#pragma once

// =====================================================================
//  The Lua 5.4 interpreter (deps/lua, MIT license) for the pipelines'
//  Script block, compiled into the translation unit that includes this
//  file - main.cpp, at its very end, so no build script needs to know
//  about it and Lua's internal macros (cast, G, ...) reach no other code.
//  Compiled as C++: Lua then raises its errors as C++ exceptions.
//  Only the libraries a sandboxed script needs are built (no io, os,
//  package / require, debug: see PipelineScript.h).
// =====================================================================

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#if !defined(__clang__)
#pragma GCC diagnostic ignored "-Wstringop-overflow"   // false alarms of GCC's inliner in luaL_addlstring
#pragma GCC diagnostic ignored "-Wrestrict"
#endif
#endif

// luaconf.h was read already (through lua.h in Pipeline.h) without LUA_CORE, so the names
// it keeps for Lua's own use are not there yet
#define LUA_CORE
#define LUA_LIB
#ifndef l_likely
#define l_likely(x) luai_likely(x)
#define l_unlikely(x) luai_unlikely(x)
#endif

#include "../../../deps/lua/lzio.c"
#include "../../../deps/lua/lctype.c"
#include "../../../deps/lua/lopcodes.c"
#include "../../../deps/lua/lmem.c"
#include "../../../deps/lua/lundump.c"
#include "../../../deps/lua/ldump.c"
#include "../../../deps/lua/lstate.c"
#include "../../../deps/lua/lgc.c"
#include "../../../deps/lua/llex.c"
#include "../../../deps/lua/lcode.c"
#include "../../../deps/lua/lparser.c"
#include "../../../deps/lua/ldebug.c"
#include "../../../deps/lua/lfunc.c"
#include "../../../deps/lua/lobject.c"
#include "../../../deps/lua/ltm.c"
#include "../../../deps/lua/lstring.c"
#include "../../../deps/lua/ltable.c"
#include "../../../deps/lua/ldo.c"
#include "../../../deps/lua/lvm.c"
#include "../../../deps/lua/lapi.c"
#include "../../../deps/lua/lauxlib.c"
#include "../../../deps/lua/lbaselib.c"
#include "../../../deps/lua/lcorolib.c"
#include "../../../deps/lua/lmathlib.c"
#include "../../../deps/lua/lstrlib.c"
#include "../../../deps/lua/ltablib.c"
#include "../../../deps/lua/lutf8lib.c"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
