#pragma once
// =====================================================================
//  Script (Lua): a block whose work is a Lua 5.4 program kept in the
//  pipeline's JSON. Included from Pipeline.h; the interpreter itself is
//  compiled at the end of main.cpp (LuaImpl.h).
//
//  What DSP blocks cannot express - frame sync by a code's syndrome
//  (RDS, POCSAG), fields spread over several frames (an RDS station name
//  in four pieces), checksums, state machines, a protocol nobody has
//  written a block for - is a few lines of Lua. An AI assistant that
//  reads docs/pipeline.md can write them into the JSON.
//
//  The script is called for every element of its inputs (up to 4):
//     on_bit(b, port)      symbols / bits (integer)
//     on_real(x, port)     real samples
//     on_iq(i, q, port)    I/Q samples
//     on_frame(s, port)    frames, as a Lua string of bytes
//     on_tick(dt)          once per pass (dt: seconds), also without input
//  and gives its results with
//     emit(s)              a frame (string, or a table of byte values) -> "frames"
//     out_real(x) / out_bit(b) / out_iq(i, q)  -> "real" / "bits" / "iq"
//     show(name, value)    a line of the block's table (kept, updated by name)
//     log(...) / print(...) a line of the block's log
//  plus rate(port), hex(s), P[1..4] (the block's numbers), ARG (its text).
//  Sandbox: no files, no OS, no modules; a pass that runs for more than
//  half a second stops with an error (an endless loop can't hang the
//  pipeline), memory is capped at 256 MB.
// =====================================================================

// (its headers - Lua's, NativeDialogs.h, <filesystem> - are included at the top of Pipeline.h,
// outside namespace pl)

class ScriptBlock : public Block {
    lua_State* L = nullptr;
    int fnBit = LUA_NOREF, fnReal = LUA_NOREF, fnIq = LUA_NOREF, fnFrame = LUA_NOREF, fnTick = LUA_NOREF;
    std::string err;                                // the last error (the script stops until the code changes or Restart)
    int loadedVersion = -1;
    std::string loadedCode;
    std::vector<std::pair<std::string, std::string>> table;   // show()
    std::deque<std::string> logLines;
    std::vector<double> inRates;
    size_t memUsed = 0;
    std::chrono::steady_clock::time_point deadline;
    long long calls = 0;
    // Edit code: the code in a file opened in the system's editor, read back when it changes
    std::string editPath;
    long long editMtime = 0;
    std::chrono::steady_clock::time_point lastFileCheck;
    static constexpr size_t MEM_LIMIT = 256u << 20;

    static ScriptBlock* self(lua_State* l) { return *(ScriptBlock**)lua_getextraspace(l); }

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
        ScriptBlock* b = (ScriptBlock*)ud;
        const size_t old = ptr ? osize : 0;
        if (nsize == 0) { b->memUsed -= old; std::free(ptr); return nullptr; }
        if (b->memUsed - old + nsize > MEM_LIMIT) return nullptr;   // Lua raises "not enough memory"
        void* p = std::realloc(ptr, nsize);
        if (p) b->memUsed = b->memUsed - old + nsize;
        return p;
    }
    static void hook(lua_State* l, lua_Debug*) {
        if (std::chrono::steady_clock::now() > self(l)->deadline) luaL_error(l, "the script took more than 0.5 s in one pass (endless loop?)");
    }

    // --- functions the script can call ---
    static std::string bytesArg(lua_State* l, int i) {
        if (lua_type(l, i) == LUA_TTABLE) {
            std::string s;
            const lua_Integer n = luaL_len(l, i);
            for (lua_Integer k = 1; k <= n; k++) { lua_geti(l, i, k); s += (char)(uint8_t)luaL_checkinteger(l, -1); lua_pop(l, 1); }
            return s;
        }
        size_t n = 0;
        const char* p = luaL_checklstring(l, i, &n);
        return std::string(p, n);
    }
    static int l_emit(lua_State* l) {
        const std::string s = bytesArg(l, 1);
        Stream& o = self(l)->out[0];
        o.frames.emplace_back(s.begin(), s.end());
        return 0;
    }
    static int l_outReal(lua_State* l) { self(l)->out[1].re.push_back((float)luaL_checknumber(l, 1)); return 0; }
    static int l_outBit(lua_State* l) { self(l)->out[2].sym.push_back((uint8_t)luaL_checkinteger(l, 1)); return 0; }
    static int l_outIq(lua_State* l) { self(l)->out[3].iq.emplace_back((float)luaL_checknumber(l, 1), (float)luaL_checknumber(l, 2)); return 0; }
    static int l_setLevels(lua_State* l) { self(l)->out[2].levels = (int)std::clamp<lua_Integer>(luaL_checkinteger(l, 1), 2, 256); return 0; }
    static int l_show(lua_State* l) {
        ScriptBlock* b = self(l);
        const std::string k = luaL_tolstring(l, 1, nullptr);
        lua_pop(l, 1);
        const std::string v = lua_isnoneornil(l, 2) ? std::string() : std::string(luaL_tolstring(l, 2, nullptr));
        for (auto& kv : b->table) if (kv.first == k) { kv.second = v; return 0; }
        if (b->table.size() < 40) b->table.push_back({k, v});
        return 0;
    }
    static int l_log(lua_State* l) {
        ScriptBlock* b = self(l);
        std::string s;
        const int n = lua_gettop(l);
        for (int i = 1; i <= n; i++) { if (i > 1) s += "  "; s += luaL_tolstring(l, i, nullptr); lua_pop(l, 1); }
        b->logLines.push_back(s);
        while (b->logLines.size() > 200) b->logLines.pop_front();
        return 0;
    }
    static int l_clearLog(lua_State* l) { self(l)->logLines.clear(); return 0; }
    static int l_rate(lua_State* l) {
        ScriptBlock* b = self(l);
        const lua_Integer p = luaL_optinteger(l, 1, 1);
        lua_pushnumber(l, p >= 1 && p <= (lua_Integer)b->inRates.size() ? b->inRates[(size_t)p - 1] : 0.0);
        return 1;
    }
    static int l_hex(lua_State* l) {
        const std::string s = bytesArg(l, 1);
        std::string h;
        char t[4];
        for (unsigned char c : s) { std::snprintf(t, sizeof(t), h.empty() ? "%02X" : " %02X", c); h += t; }
        lua_pushstring(l, h.c_str());
        return 1;
    }

    void closeState() {
        if (L) lua_close(L);
        L = nullptr;
        fnBit = fnReal = fnIq = fnFrame = fnTick = LUA_NOREF;
        memUsed = 0;
    }
    int ref(const char* name) {
        lua_getglobal(L, name);
        if (lua_isfunction(L, -1)) return luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pop(L, 1);
        return LUA_NOREF;
    }
    // a new interpreter with the script loaded and its top level run
    void load() {
        closeState();
        err.clear(); table.clear(); logLines.clear(); calls = 0;
        loadedVersion = paramVersion;
        loadedCode = text("code");
        L = lua_newstate(alloc, this);
        if (!L) { err = "could not start Lua"; return; }
        *(ScriptBlock**)lua_getextraspace(L) = this;
        // the safe libraries only: no io, os, package (require), debug
        const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
                                 {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
        for (const auto& lib : libs) { luaL_requiref(L, lib.name, lib.func, 1); lua_pop(L, 1); }
        for (const char* g : {"dofile", "loadfile", "collectgarbage"}) { lua_pushnil(L); lua_setglobal(L, g); }
        const luaL_Reg fns[] = {{"emit", l_emit}, {"out_real", l_outReal}, {"out_bit", l_outBit}, {"out_iq", l_outIq},
                                {"set_levels", l_setLevels}, {"show", l_show}, {"log", l_log}, {"print", l_log},
                                {"clear_log", l_clearLog}, {"rate", l_rate}, {"hex", l_hex}};
        for (const auto& f : fns) { lua_pushcfunction(L, f.func); lua_setglobal(L, f.name); }
        lua_createtable(L, 4, 0);
        for (int i = 1; i <= 4; i++) { lua_pushnumber(L, num("p" + std::to_string(i))); lua_rawseti(L, -2, i); }
        lua_setglobal(L, "P");
        lua_pushstring(L, text("arg").c_str());
        lua_setglobal(L, "ARG");
        lua_sethook(L, hook, LUA_MASKCOUNT, 20000);
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        if (luaL_loadbuffer(L, loadedCode.data(), loadedCode.size(), "=script") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
            lua_pop(L, 1);
            return;
        }
        fnBit = ref("on_bit"); fnReal = ref("on_real"); fnIq = ref("on_iq"); fnFrame = ref("on_frame"); fnTick = ref("on_tick");
    }
    // calls a function whose arguments are on the stack; false on an error (kept in err)
    bool call(int nargs) {
        if (lua_pcall(L, nargs, 0, 0) == LUA_OK) return true;
        err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        lua_pop(L, 1);
        return false;
    }
    // P[] and ARG follow the block's parameters without reloading the script
    void updateParams() {
        lua_getglobal(L, "P");
        if (lua_istable(L, -1)) for (int i = 1; i <= 4; i++) { lua_pushnumber(L, num("p" + std::to_string(i))); lua_rawseti(L, -2, i); }
        lua_pop(L, 1);
        lua_pushstring(L, text("arg").c_str());
        lua_setglobal(L, "ARG");
    }

    static long long fileTime(const std::string& p) {
        struct stat st;
        return stat(p.c_str(), &st) == 0 ? (long long)st.st_mtime * 1000 + (long long)st.st_size % 1000 : 0;
    }
    // the code changed in the external editor: into the parameter (saved with the pipeline)
    void pollFile() {
        if (editPath.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFileCheck < std::chrono::milliseconds(500)) return;
        lastFileCheck = now;
        const long long t = fileTime(editPath);
        if (t == 0 || t == editMtime) return;
        editMtime = t;
        std::ifstream f(editPath, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string code = ss.str();
        code.erase(std::remove(code.begin(), code.end(), '\r'), code.end());
        for (auto& p : params) if (p.key == "code" && p.text != code) { p.text = code; paramVersion++; wantsSave = true; }
    }

public:
    ScriptBlock() {
        type = "script"; title = "Script (Lua)"; category = "Code";
        ins = {{"in", PT::All}}; maxIns = 4;
        setOuts({{"frames", PT::Frames}, {"real", PT::Real}, {"bits", PT::Sym}, {"iq", PT::IQ}});
        addText("code", "Code (Lua)",
                "-- Lua 5.4. Called for every element of the inputs:\n"
                "--   on_bit(b, port)  on_real(x, port)  on_iq(i, q, port)  on_frame(s, port)  on_tick(dt)\n"
                "-- Results: emit(s) (a frame), out_real(x), out_bit(b), out_iq(i, q), show(name, value), log(...)\n"
                "local n = 0\n"
                "function on_frame(s, port)\n"
                "  n = n + 1\n"
                "  show(\"frames\", n)\n"
                "  emit(s)\n"
                "end\n").hidden = true;
        addNum("rate", "Output rate (0 = input's)", 0, 0, 1e8, 100, "S/s");
        addNum("p1", "P[1]", 0, -1e9, 1e9, 1, "", 3);
        addNum("p2", "P[2]", 0, -1e9, 1e9, 1, "", 3);
        addNum("p3", "P[3]", 0, -1e9, 1e9, 1, "", 3);
        addNum("p4", "P[4]", 0, -1e9, 1e9, 1, "", 3);
        addText("arg", "ARG (text)", "");
    }
    ~ScriptBlock() override { closeState(); }
    void reset() override { closeState(); loadedVersion = -1; }

    // the error, the table and the log as text (Copy log, tests)
    std::string summary() const {
        std::string t = err.empty() ? "" : "error: " + err + "\n";
        for (const auto& kv : table) t += kv.first + ": " + kv.second + "\n";
        for (const auto& l : logLines) t += l + "\n";
        return t;
    }
    std::vector<std::string> buttons() const override { return {"Edit code", "Restart", "Copy log"}; }
    void onButton(int i) override {
        if (i == 1) { loadedVersion = -1; return; }   // reloaded on the next pass
        if (i == 2) {
            const std::string t = summary();
            sf::Clipboard::setString(sf::String::fromUtf8(t.begin(), t.end()));
            return;
        }
        // Edit code: a file next to pipelines.json, opened in the system's editor; saving it
        // there reloads the script (pollFile)
        try {
            std::filesystem::path dir = hooks().storePath ? std::filesystem::path(hooks().storePath()).parent_path() : std::filesystem::temp_directory_path();
            dir /= "scripts";
            std::filesystem::create_directories(dir);
            if (editPath.empty()) {
                char name[64];
                std::snprintf(name, sizeof(name), "script_%d_%04x.lua", id, (unsigned)(std::rand() & 0xFFFF));
                editPath = (dir / name).string();
            }
            std::ofstream f(editPath, std::ios::binary);
            f << text("code");
            f.close();
            editMtime = fileTime(editPath);
            openWithDefaultApp(editPath);
        } catch (...) {
            err = "could not write the script file for editing";
        }
    }
    std::string status() const override {
        if (!err.empty()) return "error (see the block)";
        char b[96];
        std::snprintf(b, sizeof(b), "running, %lld calls, %.1f MB", calls, memUsed / 1048576.0);
        return b;
    }
    sf::Vector2f viewSize() const override { return {440.0f, 200.0f}; }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(6, 10, 14), sf::Color(50, 60, 75));
        float y = r.position.y + 3;
        auto line = [&](std::string s, sf::Color c) {
            if (y > r.position.y + r.size.y - 13) return;
            while (!s.empty() && d.width(s, 10) > r.size.x - 8) {
                unsigned char ch;
                do { ch = (unsigned char)s.back(); s.pop_back(); } while (!s.empty() && (ch & 0xC0) == 0x80);
            }
            d.text(s, r.position.x + 4, y, 10, c);
            y += 13.0f;
        };
        if (!err.empty()) {
            // the message can be long: a few lines of it
            std::string e = err;
            for (int k = 0; k < 4 && !e.empty(); k++) {
                size_t cut = e.size();
                while (cut > 1 && d.width(e.substr(0, cut), 10) > r.size.x - 8) cut--;
                line(e.substr(0, cut), sf::Color(255, 120, 110));
                e.erase(0, cut);
            }
        }
        for (const auto& kv : table) line(kv.first + ": " + kv.second, sf::Color(255, 210, 120));
        if (!table.empty() && !logLines.empty()) y += 3;
        const int rows = std::max(0, (int)((r.position.y + r.size.y - y - 2) / 13.0f));
        const size_t from = logLines.size() > (size_t)rows ? logLines.size() - rows : 0;
        for (size_t k = from; k < logLines.size(); k++) line(logLines[k], sf::Color(170, 230, 180));
        if (err.empty() && table.empty() && logLines.empty()) line("(nothing shown yet: show() / log() in the script)", sf::Color(110, 110, 120));
    }

    void process(const std::vector<const Stream*>& in) override {
        for (auto& o : out) o.clear();
        out[0].type = PT::Frames; out[1].type = PT::Real; out[2].type = PT::Sym; out[3].type = PT::IQ;
        pollFile();
        if (loadedVersion < 0 || text("code") != loadedCode) load();
        else if (loadedVersion != paramVersion && L) { updateParams(); loadedVersion = paramVersion; }
        inRates.assign(in.size(), 0.0);
        double first = 0.0;
        for (size_t k = 0; k < in.size(); k++) if (in[k]) { inRates[k] = in[k]->rate; if (first <= 0) first = in[k]->rate; }
        const double outRate = num("rate") > 0 ? num("rate") : first;
        for (auto& o : out) o.rate = outRate;
        out[0].rate = first;
        if (!L || !err.empty()) return;
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        for (size_t k = 0; k < in.size(); k++) {
            const Stream* s = in[k];
            if (!s) continue;
            const lua_Integer port = (lua_Integer)k + 1;
            switch (s->type) {
            case PT::Sym:
                if (fnBit == LUA_NOREF) break;
                for (uint8_t v : s->sym) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, fnBit); lua_pushinteger(L, v); lua_pushinteger(L, port);
                    calls++;
                    if (!call(2)) return;
                }
                break;
            case PT::Real:
                if (fnReal == LUA_NOREF) break;
                for (float v : s->re) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, fnReal); lua_pushnumber(L, v); lua_pushinteger(L, port);
                    calls++;
                    if (!call(2)) return;
                }
                break;
            case PT::IQ:
                if (fnIq == LUA_NOREF) break;
                for (const CF& v : s->iq) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, fnIq); lua_pushnumber(L, v.real()); lua_pushnumber(L, v.imag()); lua_pushinteger(L, port);
                    calls++;
                    if (!call(3)) return;
                }
                break;
            case PT::Frames:
                if (fnFrame == LUA_NOREF) break;
                for (const auto& f : s->frames) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, fnFrame); lua_pushlstring(L, (const char*)f.data(), f.size()); lua_pushinteger(L, port);
                    calls++;
                    if (!call(2)) return;
                }
                break;
            default:
                break;
            }
        }
        if (fnTick != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, fnTick); lua_pushnumber(L, tick);
            if (!call(1)) return;
        }
        if (!out[0].frames.empty()) out[0].newBurst = true;
    }
};
