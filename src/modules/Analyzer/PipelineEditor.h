#pragma once

#include "Pipeline.h"
#include <SFML/Graphics.hpp>
#include <functional>
#include <string>
#include <vector>
#include <cmath>
#include <set>
#include <unordered_map>

// =====================================================================
//  The pipeline editor: a full-window workspace (a tab next to the
//  receiver tabs).
//
//   top:    the chain (pick / new / duplicate / rename / delete), the
//           presets, import / export (JSON)
//   left:   the blocks (click to add)
//   canvas: the blocks and their wires. Drag a block by its title, a
//           wire from an output (right) to an input (left); press on a
//           connected input to pull its wire off; right-click an input
//           to disconnect. Numbers: drag sideways, mouse wheel, or click
//           and type. Choices: click (right-click: back). Del: remove the
//           selected block. Drag the empty canvas to move around.
//
//  Every change is saved (pipelines.json in the settings folder) a
//  moment later.
// =====================================================================

namespace pl {


class PipelineEditor {
    Pipeline& pipe;
    const sf::Font* font = nullptr;
    sf::FloatRect area;               // the whole workspace (window coordinates)
    sf::FloatRect canvas, palette, toolbar;
    sf::Vector2f pan {0.0f, 0.0f};
    int selected = -1;

    enum class Drag { None, Node, Pan, Link, Value, Palette };
    std::string dragType;             // Palette: the block being dragged in
    Drag drag = Drag::None;
    int dragNode = -1, dragOut = 0, dragParam = -1;
    sf::Vector2f dragStart, dragOffset, mouse;
    double dragValue = 0.0;
    bool dragMoved = false;

    // typing into a parameter or the chain's name
    int editNode = -1, editParam = -1;
    bool editName = false;
    std::string editBuf;

    // menus
    int menu = 0;                     // 1 chains, 2 presets
    std::vector<std::pair<sf::FloatRect, int>> menuHits;
    std::vector<std::pair<sf::FloatRect, std::string>> paletteHits;
    struct Btn { sf::FloatRect r; std::string id; };
    std::vector<Btn> toolHits;

    // zoom of the canvas, the mouse in the canvas' coordinates
    float zoom = 1.0f;
    sf::Vector2f mouseC;
    // the palette: search, folded categories, scroll
    std::string search;
    bool searchFocus = false;
    int searchSel = 0;            // the highlighted match (arrow keys, Enter adds it)
    std::string tipType;          // the palette block under the mouse (its keywords are shown beside it)
    sf::Vector2f tipAt;
    float paletteScroll = 0.0f;
    std::set<std::string> collapsed;
    std::vector<std::pair<sf::FloatRect, std::string>> catHits;
    sf::FloatRect searchBox;
    // the list of a choice parameter (menu 3)
    int menuNode = -1, menuParam = -1;
    sf::Vector2f menuAt;
    bool showHelp = false;
    sf::Clock lastDraw, choicesClock;

    // saving
    bool dirty = false;
    sf::Clock dirtyClock, msgClock;
    std::string msg;
    bool initialised = false;
    float addCascade = 0.0f;

    // ---------------- geometry ----------------
    struct Geom {
        sf::FloatRect box, head;
        std::vector<sf::FloatRect> params, buttons;
        sf::FloatRect view;
        std::vector<sf::Vector2f> ins, outs;
        bool hasStatus = false;
    };
    static constexpr float HEAD = 22.0f, ROW = 17.0f, STATUS = 15.0f, BTN = 22.0f;

    sf::Vector2f origin() const { return {canvas.position.x + pan.x, canvas.position.y + pan.y}; }

    Geom geom(const Block& b) const {
        Geom g;
        const sf::Vector2f vs = b.viewSize();
        const float w = std::max(210.0f, vs.x + 16.0f);
        const sf::Vector2f o = origin();
        const float x = o.x + b.x, y = o.y + b.y;
        float cy = y + HEAD;
        g.hasStatus = !b.status().empty() || b.isInput;
        if (g.hasStatus) cy += STATUS;
        if (!b.warning.empty()) cy += STATUS;
        if (!b.note.empty()) cy += STATUS;
        for (size_t i = 0; i < b.params.size(); i++) {
            // a parameter that does not apply now (e.g. Kaiser beta with a Hamming window): no row
            if (!b.shown(b.params[i])) { g.params.push_back(sf::FloatRect({x, cy}, {0, 0})); continue; }
            g.params.push_back(sf::FloatRect({x + 8, cy}, {w - 16, ROW}));
            cy += ROW;
        }
        const auto bt = b.buttons();
        if (!bt.empty()) {
            cy += 3;
            float bx = x + 8;
            for (const auto& s : bt) {
                const float bw = std::max(50.0f, measure(s, 11) + 16.0f);
                g.buttons.push_back(sf::FloatRect({bx, cy}, {bw, BTN - 4}));
                bx += bw + 6;
            }
            cy += BTN;
        }
        if (vs.x > 0) { g.view = sf::FloatRect({x + 8, cy + 4}, vs); cy += vs.y + 8; }
        const float portsH = HEAD + 10 + std::max(b.ins.size(), b.outs.size()) * 18.0f;
        const float h = std::max(cy - y + 6, portsH);
        g.box = sf::FloatRect({x, y}, {w, h});
        g.head = sf::FloatRect({x, y}, {w, HEAD});
        for (size_t i = 0; i < b.ins.size(); i++) g.ins.push_back({x, y + HEAD + 10 + i * 18.0f});
        for (size_t i = 0; i < b.outs.size(); i++) g.outs.push_back({x + w, y + HEAD + 10 + i * 18.0f});
        return g;
    }

    mutable std::unordered_map<std::string, float> widths;   // measure(): text width by (size, text)
    float measure(const std::string& s, unsigned size) const {
        if (!font) return 0.0f;
        std::string k(1, (char)size);
        k += s;
        const auto it = widths.find(k);
        if (it != widths.end()) return it->second;
        if (widths.size() > 20000) widths.clear();
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size * 2);
        return widths[k] = t.getGlobalBounds().size.x * 0.5f;
    }

    void markDirty() { dirty = true; dirtyClock.restart(); }
    void say(const std::string& m) { msg = m; msgClock.restart(); }

    // ---------------- store ----------------
    PipelineStore& store() { return PipelineStore::get(); }

    void ensureStore() {
        if (initialised) return;
        initialised = true;
        auto& st = store();
        if (!st.loaded && hooks().storePath) st.load(hooks().storePath());
        if (st.items.empty()) {
            orbit::JsonValue v;
            if (orbit::parseJson(presets()[0].json, v)) {
                Pipeline tmp;
                std::string err;
                tmp.fromJson(v, err);
                tmp.name = "My first chain";
                st.put(tmp.name, tmp.toJson());
                st.save();
            }
        }
        // a pipeline not edited yet (only the input) takes the first saved one
        bool fresh;
        { std::lock_guard<std::mutex> l(pipe.mtx); fresh = pipe.nodes.size() <= 1 && pipe.name == "Untitled"; }
        if (fresh && !st.items.empty()) loadNamed(st.items[0].name);
    }

    void saveNow() {
        std::string name, json;
        { std::lock_guard<std::mutex> l(pipe.mtx); name = pipe.name; json = pipe.toJson(); }
        store().put(name, json);
        if (store().save()) dirty = false;
        else say("Could not write " + store().path);
    }

    bool loadJson(const std::string& json, const std::string& name) {
        orbit::JsonValue v;
        if (!orbit::parseJson(json, v)) return false;
        std::string err;
        std::lock_guard<std::mutex> l(pipe.mtx);
        if (!pipe.fromJson(v, err)) return false;
        if (!name.empty()) pipe.name = name;
        selected = -1;
        pan = {0, 0};
        return true;
    }
    void loadNamed(const std::string& name) {
        const int i = store().indexOf(name);
        if (i < 0) return;
        if (name != currentName() && chainInUse && chainInUse(name)) { say("\"" + name + "\" is open in another tab"); return; }
        if (dirty) saveNow();
        if (!loadJson(store().items[i].json, name)) say("\"" + name + "\" could not be read");
    }

    std::string currentName() { std::lock_guard<std::mutex> l(pipe.mtx); return pipe.name; }

    void toolbarAction(const std::string& id) {
        auto& st = store();
        if (id == "chains") { menu = menu == 1 ? 0 : 1; return; }
        if (id == "help") { showHelp = !showHelp; return; }
        if (id == "presets") { menu = menu == 2 ? 0 : 2; return; }
        menu = 0;
        if (id == "new") {
            if (dirty) saveNow();
            const std::string n = st.uniqueName("New chain");
            { std::lock_guard<std::mutex> l(pipe.mtx); pipe.clear(); pipe.name = n; }
            selected = -1; pan = {0, 0};
            saveNow();
            say("New chain \"" + n + "\"");
        } else if (id == "dup") {
            saveNow();
            const std::string n = st.uniqueName(currentName() + " copy");
            { std::lock_guard<std::mutex> l(pipe.mtx); pipe.name = n; }
            saveNow();
            say("Copied as \"" + n + "\"");
        } else if (id == "rename") {
            editName = true; editNode = -1; editBuf = currentName();
        } else if (id == "delete") {
            const std::string n = currentName();
            st.erase(n);
            int next = -1;
            for (size_t k = 0; k < st.items.size() && next < 0; k++) if (!chainInUse || !chainInUse(st.items[k].name)) next = (int)k;
            if (next < 0) {
                const std::string nn = st.uniqueName("New chain");
                { std::lock_guard<std::mutex> l(pipe.mtx); pipe.clear(); pipe.name = nn; }
                saveNow();
            } else {
                loadJson(st.items[next].json, st.items[next].name);
                st.save();
            }
            dirty = false;
            say("Deleted \"" + n + "\"");
        } else if (id == "export") {
            if (!hooks().saveJsonDialog) return;
            saveNow();
            const std::string n = currentName();
            const std::string path = hooks().saveJsonDialog(n);
            if (path.empty()) return;
            std::string json;
            { std::lock_guard<std::mutex> l(pipe.mtx); json = pipe.toJson(); }
            std::ofstream f(path, std::ios::binary);
            if (f && (f << json << "\n")) say("Exported to " + path);
            else say("Could not write " + path);
        } else if (id == "exportall") {
            if (!hooks().saveJsonDialog) return;
            saveNow();
            const std::string path = hooks().saveJsonDialog("OrbitSDR pipelines");
            if (path.empty()) return;
            std::ifstream in(st.path, std::ios::binary);
            std::ofstream f(path, std::ios::binary);
            if (in && f && (f << in.rdbuf())) say(std::to_string(st.items.size()) + " chains exported to " + path);
            else say("Could not write " + path);
        } else if (id == "import") {
            if (!hooks().openJsonDialog) return;
            const std::string path = hooks().openJsonDialog();
            if (path.empty()) return;
            std::ifstream f(path, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            orbit::JsonValue v;
            if (!orbit::parseJson(ss.str(), v)) { say("Not a JSON file: " + path); return; }
            const size_t before = st.items.size();
            const int n = st.importValue(v, true);
            if (n <= 0) { say("No pipeline in " + path); return; }
            st.save();
            if (dirty) saveNow();
            loadJson(st.items[before].json, st.items[before].name);
            say("Imported " + std::to_string(n) + (n == 1 ? " chain" : " chains"));
        }
    }

    void addBlock(const std::string& type) {
        std::lock_guard<std::mutex> l(pipe.mtx);
        // in view, a little apart from the last one added
        const float x = -pan.x + 260.0f + addCascade, y = -pan.y + 40.0f + addCascade;
        addCascade = std::fmod(addCascade + 26.0f, 182.0f);
        if (Block* b = pipe.add(type, x, y)) selected = b->id;
        markDirty();
    }
    // dropped from the palette: the title bar under the mouse
    void addBlockAt(const std::string& type, sf::Vector2f m) {
        std::lock_guard<std::mutex> l(pipe.mtx);
        const sf::Vector2f o = origin();
        if (Block* b = pipe.add(type, m.x - o.x - 30.0f, m.y - o.y - 10.0f)) selected = b->id;
        markDirty();
    }

    void applyEdit() {
        if (editName) {
            std::string n = editBuf;
            while (!n.empty() && n.back() == ' ') n.pop_back();
            const std::string old = currentName();
            if (!n.empty() && n != old) {
                if (store().indexOf(n) >= 0) say("\"" + n + "\" exists already");
                else {
                    store().erase(old);
                    { std::lock_guard<std::mutex> l(pipe.mtx); pipe.name = n; }
                    saveNow();
                    say("Renamed to \"" + n + "\"");
                }
            }
            editName = false;
            return;
        }
        if (editNode < 0) return;
        std::lock_guard<std::mutex> l(pipe.mtx);
        if (Block* b = pipe.find(editNode)) {
            if (editParam >= 0 && editParam < (int)b->params.size()) {
                Param& p = b->params[editParam];
                if (p.kind == Param::Text) p.text = editBuf;
                else {
                    char* end = nullptr;
                    const double v = std::strtod(editBuf.c_str(), &end);
                    if (end != editBuf.c_str()) p.v = std::clamp(v, p.lo, p.hi);
                }
                b->paramVersion++;
                markDirty();
            }
        }
        editNode = -1; editParam = -1;
    }

    void stepParam(Block* b, int pi, double steps) {
        Param& p = b->params[pi];
        if (p.kind == Param::Choice) {
            const int n = (int)p.choices.size();
            p.v = (double)((((int)p.v + (int)steps) % n + n) % n);
        } else if (p.kind == Param::Num) {
            p.v = std::clamp(p.v + steps * p.step, p.lo, p.hi);
        } else return;
        b->paramVersion++;
        markDirty();
    }

    // what is under the mouse
    struct Hit { int node = -1; int kind = 0; int idx = -1; };   // kind: 1 head 2 in-port 3 out-port 4 param 5 button 6 body
    Hit hitTest(sf::Vector2f m) {
        Hit h;
        for (auto it = pipe.nodes.rbegin(); it != pipe.nodes.rend(); ++it) {
            const Block& b = **it;
            const Geom g = geom(b);
            for (size_t i = 0; i < g.ins.size(); i++)
                if (std::hypot(m.x - g.ins[i].x, m.y - g.ins[i].y) < 9) return {b.id, 2, (int)i};
            for (size_t i = 0; i < g.outs.size(); i++)
                if (std::hypot(m.x - g.outs[i].x, m.y - g.outs[i].y) < 9) return {b.id, 3, (int)i};
            if (!g.box.contains(m)) continue;
            if (g.head.contains(m)) return {b.id, 1, -1};
            for (size_t i = 0; i < g.params.size(); i++) if (g.params[i].contains(m)) return {b.id, 4, (int)i};
            for (size_t i = 0; i < g.buttons.size(); i++) if (g.buttons[i].contains(m)) return {b.id, 5, (int)i};
            return {b.id, 6, -1};
        }
        return h;
    }

    // ---------------- drawing helpers ----------------
    Draw dr(sf::RenderTarget& w) const { Draw d; d.font = font; d.win = &w; return d; }

    void bezier(sf::RenderTarget& w, sf::Vector2f a, sf::Vector2f b, sf::Color c) const {
        const float dx = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f);
        sf::VertexArray va(sf::PrimitiveType::TriangleStrip);
        const int N = 32;
        sf::Vector2f prev = a;
        for (int i = 0; i <= N; i++) {
            const float t = (float)i / N, u = 1 - t;
            const sf::Vector2f p = u * u * u * a + 3 * u * u * t * sf::Vector2f(a.x + dx, a.y) + 3 * u * t * t * sf::Vector2f(b.x - dx, b.y) + t * t * t * b;
            sf::Vector2f d = p - prev;
            if (i == 0) d = {1, 0};
            const float len = std::max(1e-3f, std::hypot(d.x, d.y));
            const sf::Vector2f nrm(-d.y / len * 1.3f, d.x / len * 1.3f);
            va.append({p + nrm, c}); va.append({p - nrm, c});
            prev = p;
        }
        w.draw(va);
    }

    void port(sf::RenderTarget& w, sf::Vector2f p, PT t, bool hot, const sf::Color* own = nullptr) const {
        sf::CircleShape c(hot ? 6.0f : 5.0f);
        c.setOrigin({c.getRadius(), c.getRadius()});
        c.setPosition(p);
        c.setFillColor(own ? *own : ptColor(t));
        c.setOutlineColor(sf::Color(20, 20, 24));
        c.setOutlineThickness(1.5f);
        w.draw(c);
    }

    void button(const Draw& d, const sf::FloatRect& r, const std::string& s, bool hot, bool active = false) const {
        d.rect(r, active ? sf::Color(120, 70, 10) : hot ? sf::Color(60, 64, 76) : sf::Color(40, 42, 50), sf::Color(80, 80, 92));
        d.text(s, r.position.x + (r.size.x - measure(s, 11)) / 2, r.position.y + (r.size.y - 14) / 2, 11, sf::Color(225, 225, 232));
    }

public:
    // put a query into the palette's search box (as if typed)
    void findBlocks(const std::string& q) { search = q; searchFocus = !q.empty(); searchSel = 0; paletteScroll = 0; }
    explicit PipelineEditor(Pipeline& p) : pipe(p) {}
    void setFont(const sf::Font* f) { font = f; }
    std::string title() { return currentName(); }

    // set by the workspaces: true when another tab has the chain `name` open
    std::function<bool(const std::string& name)> chainInUse;
    // Opens the saved chain `name` (false: there is none)
    bool openChain(const std::string& name) {
        ensureStore();
        const int i = store().indexOf(name);
        if (i < 0) return false;
        if (dirty) saveNow();
        return loadJson(store().items[i].json, name);
    }
    // A new empty chain (only the input), saved as base, base (2) ...
    void newChain(const std::string& base) {
        ensureStore();
        if (dirty) saveNow();
        const std::string n = store().uniqueName(base);
        { std::lock_guard<std::mutex> l(pipe.mtx); pipe.clear(); pipe.name = n; }
        selected = -1; pan = {0, 0};
        saveNow();
    }
    // The main Channel input takes this receiver ("RX2 ..." or just "RX2"; "" = none)
    void setSource(const std::string& rx) {
        {
            std::lock_guard<std::mutex> l(pipe.mtx);
            Block* in = pipe.input();
            if (!in) return;
            in->refreshChoices();
            for (auto& p : in->params) {
                if (p.key != "src") continue;
                const std::string key = rx.substr(0, rx.find(' '));
                int idx = 0;
                for (size_t c = 0; c < p.choices.size(); c++) if (!key.empty() && p.choices[c].substr(0, p.choices[c].find(' ')) == key) idx = (int)c;
                p.v = idx;
                in->paramVersion++;
            }
        }
        markDirty();
    }
    void saveIfChanged() { if (dirty) saveNow(); }

    // Called when the tab is shown (loads the saved chains on first use)
    void open() { ensureStore(); }

    // ---------------- events (window coordinates) ----------------
    // The canvas can be zoomed: its "world" coordinates (the blocks' geometry) relate to the
    // window by canvas.position + (world - canvas.position) * zoom.
    sf::Vector2f toCanvas(sf::Vector2f m) const { return canvas.position + (m - canvas.position) / zoom; }
    sf::Vector2f toScreen(sf::Vector2f c) const { return canvas.position + (c - canvas.position) * zoom; }

    // lower-case words of a text (letters and digits; everything else separates)
    static std::vector<std::string> words(const std::string& s) {
        std::vector<std::string> w;
        std::string cur;
        for (char ch : s) {
            const unsigned char c = (unsigned char)ch;
            if (std::isalnum(c)) cur += (char)std::tolower(c);
            else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) w.push_back(cur);
        return w;
    }
    // edit distance, stopping early above the limit
    static int editDistance(const std::string& a, const std::string& b, int limit) {
        if (std::abs((int)a.size() - (int)b.size()) > limit) return limit + 1;
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++) prev[j] = (int)j;
        for (size_t i = 1; i <= a.size(); i++) {
            cur[0] = (int)i;
            int best = cur[0];
            for (size_t j = 1; j <= b.size(); j++) {
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] != b[j - 1])});
                best = std::min(best, cur[j]);
            }
            if (best > limit) return limit + 1;
            std::swap(prev, cur);
        }
        return prev[b.size()];
    }

    // A palette row: the block, and while searching, the keyword that found it
    struct PalItem { const BlockInfo* bi; std::string hint; int score = 0; bool guess = false; };

    // All blocks (no search), or the matches ranked best first. Every word of the query has
    // to match the title, the category, the type or one of the block's hidden tags; a word
    // that matches nothing exactly may be one typo away from a tag (4+ letters).
    std::vector<PalItem> paletteItems() const {
        std::vector<PalItem> v;
        const auto q = words(search);
        for (const auto& bi : blockList()) {
            if (q.empty()) { v.push_back({&bi, "", 0}); continue; }
            const auto tw = words(bi.title), tg = words(blockTags(bi.type));
            std::string titleLow = bi.title, catLow = bi.category;
            for (auto& c : titleLow) c = (char)std::tolower((unsigned char)c);
            for (auto& c : catLow) c = (char)std::tolower((unsigned char)c);
            std::string titleCompact;   // "I/Q swap" -> "iqswap", so "iq" finds it
            for (char c : titleLow) if (std::isalnum((unsigned char)c)) titleCompact += c;
            int total = 0;
            std::string hint;
            bool all = true, guess = false;
            for (const auto& w : q) {
                int s = 0;
                std::string why;
                for (const auto& t : tw) if (t.rfind(w, 0) == 0) s = std::max(s, t == w ? 12 : 10);
                if (!s && (titleLow.find(w) != std::string::npos || titleCompact.find(w) != std::string::npos)) s = 7;
                if (w == bi.type) s = std::max(s, 12);
                if (!s && catLow.rfind(w, 0) == 0) s = 5;
                if (!s) for (const auto& t : tg) if (t.rfind(w, 0) == 0) { s = t == w ? 6 : 5; why = t; break; }
                if (!s) for (const auto& t : tg) if (w.size() >= 3 && t.find(w) != std::string::npos) { s = 3; why = t; break; }
                if (s < 8 && w.size() >= 4) {
                    // one typo (two in long words) from a title word counts more than a keyword
                    // ("filtr" -> "filter"); one from a keyword only when nothing else matched
                    const int lim = w.size() >= 7 ? 2 : 1;
                    for (const auto& t : tw) if (t.size() >= 4 && editDistance(w, t, lim) <= lim) { s = 8; why.clear(); break; }
                    if (!s) for (const auto& t : tg) if (editDistance(w, t, lim) <= lim) { s = 1; why = t; break; }
                }
                if (!s) { all = false; break; }
                total += s;
                guess |= s == 1;
                if (hint.empty() && !why.empty()) hint = why;
            }
            if (!all) continue;
            v.push_back({&bi, hint, total, guess});
        }
        if (!q.empty()) {
            // typo-matched keywords only when nothing matched properly
            if (std::any_of(v.begin(), v.end(), [](const PalItem& p) { return !p.guess; }))
                v.erase(std::remove_if(v.begin(), v.end(), [](const PalItem& p) { return p.guess; }), v.end());
            // best score first; ties keep the palette's order
            std::vector<size_t> idx(v.size());
            for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return v[a].score > v[b].score; });
            std::vector<PalItem> r;
            for (size_t i : idx) r.push_back(v[i]);
            v.swap(r);
        }
        return v;
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        if (!font) return false;
        ensureStore();
        auto toWin = [&](sf::Vector2i p) { return win.mapPixelToCoords(p); };

        // typing into the palette's search box
        if (searchFocus) {
            if (const auto* te = ev.getIf<sf::Event::TextEntered>()) {
                const char32_t c = te->unicode;
                if (c == 8) { if (!search.empty()) search.pop_back(); }
                else if (c == 27) { search.clear(); searchFocus = false; }
                else if (c >= 32 && c < 127) search += (char)c;
                paletteScroll = 0;
                searchSel = 0;
                return true;
            }
            if (const auto* kp = ev.getIf<sf::Event::KeyPressed>()) {
                const auto items = paletteItems();
                if (kp->code == sf::Keyboard::Key::Escape) { search.clear(); searchFocus = false; searchSel = 0; }
                // arrows pick a match, Enter adds it
                if (kp->code == sf::Keyboard::Key::Down) searchSel = std::min(searchSel + 1, std::max(0, (int)items.size() - 1));
                if (kp->code == sf::Keyboard::Key::Up) searchSel = std::max(searchSel - 1, 0);
                if (kp->code == sf::Keyboard::Key::Enter && !items.empty())
                    addBlock(items[std::clamp(searchSel, 0, (int)items.size() - 1)].bi->type);
                return true;
            }
            if (ev.is<sf::Event::MouseButtonPressed>()) searchFocus = false;   // and go on with the click
        }

        // typing into a parameter or the chain's name
        if (editName || editNode >= 0) {
            if (const auto* te = ev.getIf<sf::Event::TextEntered>()) {
                const char32_t c = te->unicode;
                if (c == 8) { while (!editBuf.empty() && ((unsigned char)editBuf.back() & 0xC0) == 0x80) editBuf.pop_back(); if (!editBuf.empty()) editBuf.pop_back(); }
                else if (c == 13 || c == 10) applyEdit();
                else if (c >= 32 && c != 127) {
                    sf::String s(c);
                    const auto u = s.toUtf8();
                    editBuf.append(u.begin(), u.end());
                }
                return true;
            }
            if (const auto* kp = ev.getIf<sf::Event::KeyPressed>()) {
                if (kp->code == sf::Keyboard::Key::Escape) { editName = false; editNode = -1; }
                else if (kp->code == sf::Keyboard::Key::Enter) applyEdit();
                return true;
            }
            if (ev.is<sf::Event::MouseButtonPressed>()) applyEdit();   // and go on with the click
        }

        if (const auto* mm = ev.getIf<sf::Event::MouseMoved>()) {
            mouse = toWin(mm->position);
            mouseC = toCanvas(mouse);
            std::lock_guard<std::mutex> l(pipe.mtx);
            if (drag == Drag::Node) {
                if (Block* b = pipe.find(dragNode)) { b->x = mouseC.x - origin().x - dragOffset.x; b->y = mouseC.y - origin().y - dragOffset.y; markDirty(); }
            } else if (drag == Drag::Pan) {
                pan += (mouse - dragStart) / zoom; dragStart = mouse;
            } else if (drag == Drag::Palette) {
                if (std::hypot(mouse.x - dragStart.x, mouse.y - dragStart.y) > 4) dragMoved = true;
            } else if (drag == Drag::Value) {
                if (Block* b = pipe.find(dragNode)) {
                    Param& p = b->params[dragParam];
                    const float dx = mouse.x - dragStart.x;
                    if (std::fabs(dx) > 3) dragMoved = true;
                    if (dragMoved) {
                        const double v = std::clamp(dragValue + std::round(dx / 4.0) * p.step, p.lo, p.hi);
                        if (v != p.v) { p.v = v; b->paramVersion++; markDirty(); }
                    }
                }
            }
            return area.contains(mouse);
        }

        if (const auto* mr = ev.getIf<sf::Event::MouseButtonReleased>()) {
            mouse = toWin(mr->position);
            mouseC = toCanvas(mouse);
            if (drag == Drag::Palette) {
                // dragged onto the canvas: it appears under the mouse; a plain click: next to the last
                if (dragMoved && canvas.contains(mouse)) addBlockAt(dragType, mouseC);
                else if (!dragMoved) addBlock(dragType);
            } else if (drag == Drag::Link) {
                std::lock_guard<std::mutex> l(pipe.mtx);
                const Hit h = hitTest(mouseC);
                if (h.kind == 2) {
                    const std::string err = pipe.connect(dragNode, dragOut, h.node, h.idx);
                    if (!err.empty()) say(err); else markDirty();
                }
            } else if (drag == Drag::Value && !dragMoved) {
                std::lock_guard<std::mutex> l(pipe.mtx);
                if (Block* b = pipe.find(dragNode)) {
                    Param& p = b->params[dragParam];
                    if (p.kind == Param::Num) {
                        editNode = dragNode; editParam = dragParam;
                        char buf[48];
                        std::snprintf(buf, sizeof(buf), "%.*f", p.decimals, p.v);
                        editBuf = buf;
                    }
                }
            }
            const bool was = drag != Drag::None;
            drag = Drag::None;
            return was || area.contains(mouse);
        }

        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            const sf::Vector2f m = toWin(sc->position);
            if (!area.contains(m)) return false;
            if (palette.contains(m)) { paletteScroll = std::max(0.0f, paletteScroll - sc->delta * 48.0f); return true; }
            if (canvas.contains(m)) {
                const bool ctrl = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RControl);
                std::lock_guard<std::mutex> l(pipe.mtx);
                const sf::Vector2f mc = toCanvas(m);
                const Hit h = hitTest(mc);
                if (h.kind == 4 && !ctrl) {
                    if (Block* b = pipe.find(h.node)) {
                        const bool fine = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift);
                        stepParam(b, h.idx, (sc->delta > 0 ? 1.0 : -1.0) * (fine ? 0.1 : 1.0));
                    }
                } else if (ctrl) {
                    // zoom about the mouse: the point under it stays
                    const float nz = std::clamp(zoom * (sc->delta > 0 ? 1.12f : 1.0f / 1.12f), 0.3f, 2.0f);
                    pan += (mc - canvas.position) * (zoom / nz - 1.0f);
                    zoom = nz;
                } else {
                    pan.y += sc->delta * 40.0f / zoom;   // scroll the canvas
                }
            }
            return true;
        }

        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            const sf::Vector2f m = toWin(mb->position);
            mouse = m;
            mouseC = toCanvas(m);
            if (!area.contains(m)) { menu = 0; return false; }
            const bool left = mb->button == sf::Mouse::Button::Left;
            const bool right = mb->button == sf::Mouse::Button::Right;
            // menus first
            if (menu) {
                for (const auto& mh : menuHits) {
                    if (!mh.first.contains(m)) continue;
                    if (menu == 1) {
                        if (mh.second < 0) toolbarAction("new");
                        else if (mh.second < (int)store().items.size()) loadNamed(store().items[mh.second].name);
                    } else if (menu == 2 && mh.second < (int)presets().size()) {
                        if (dirty) saveNow();
                        const auto& pr = presets()[mh.second];
                        orbit::JsonValue v;
                        if (orbit::parseJson(pr.json, v)) {
                            const std::string n = store().uniqueName(pr.name);
                            std::string src;
                            { std::lock_guard<std::mutex> l(pipe.mtx); if (Block* in = pipe.input()) if (const Param* sp = in->param("src")) src = sp->show(); }
                            loadJson(pr.json, n);
                            if (!src.empty()) setSource(src);
                            saveNow();
                            say("Preset loaded as \"" + n + "\"");
                        }
                    } else if (menu == 3) {
                        std::lock_guard<std::mutex> l(pipe.mtx);
                        if (Block* b = pipe.find(menuNode)) if (menuParam < (int)b->params.size()) {
                            b->params[menuParam].v = mh.second; b->paramVersion++; markDirty();
                        }
                    }
                    menu = 0;
                    return true;
                }
                menu = 0;
                return true;
            }
            if (toolbar.contains(m)) {
                for (const auto& b : toolHits) if (b.r.contains(m) && left) { toolbarAction(b.id); return true; }
                return true;
            }
            if (palette.contains(m)) {
                if (searchBox.contains(m)) { searchFocus = true; return true; }
                for (const auto& c : catHits) if (c.first.contains(m) && left) {
                    if (collapsed.count(c.second)) collapsed.erase(c.second); else collapsed.insert(c.second);
                    return true;
                }
                for (const auto& p : paletteHits)
                    if (p.first.contains(m) && left) { drag = Drag::Palette; dragType = p.second; dragStart = m; dragMoved = false; return true; }
                return true;
            }
            if (!canvas.contains(m)) return true;
            std::lock_guard<std::mutex> l(pipe.mtx);
            const sf::Vector2f mc = mouseC;
            const Hit h = hitTest(mc);
            if (h.node < 0) {
                if (left || mb->button == sf::Mouse::Button::Middle) { drag = Drag::Pan; dragStart = m; }
                selected = -1;
                return true;
            }
            Block* b = pipe.find(h.node);
            if (!b) return true;
            selected = h.node;
            // bring to front
            for (size_t i = 0; i < pipe.nodes.size(); i++) if (pipe.nodes[i].get() == b && i + 1 < pipe.nodes.size()) {
                auto keep = std::move(pipe.nodes[i]);
                pipe.nodes.erase(pipe.nodes.begin() + i);
                pipe.nodes.push_back(std::move(keep));
                break;
            }
            const Geom g = geom(*b);
            switch (h.kind) {
            case 1:
                if (left) {
                    // the x in the title bar
                    if (b->removable && mc.x > g.head.position.x + g.head.size.x - 20) { pipe.remove(b->id); selected = -1; markDirty(); return true; }
                    drag = Drag::Node; dragNode = b->id; dragOffset = {mc.x - g.box.position.x, mc.y - g.box.position.y};
                }
                return true;
            case 2:
                if (right) { pipe.disconnect(b->id, h.idx); markDirty(); return true; }
                if (left) {
                    // pull an existing wire off this input
                    if (const Link* lk = pipe.linkInto(b->id, h.idx)) {
                        drag = Drag::Link; dragNode = lk->from; dragOut = lk->out;
                        pipe.disconnect(b->id, h.idx); markDirty();
                    }
                }
                return true;
            case 3:
                if (left) { drag = Drag::Link; dragNode = b->id; dragOut = h.idx; }
                return true;
            case 4: {
                Param& p = b->params[h.idx];
                if (p.kind == Param::Choice) {
                    // a list to pick from (right-click: the previous one)
                    if (right) stepParam(b, h.idx, -1);
                    else { menu = 3; menuNode = b->id; menuParam = h.idx; menuAt = toScreen({g.params[h.idx].position.x, g.params[h.idx].position.y + ROW}); }
                    return true;
                }
                if (p.kind == Param::Text) { if (left) { editNode = b->id; editParam = h.idx; editBuf = p.text; } return true; }
                if (left) { drag = Drag::Value; dragNode = b->id; dragParam = h.idx; dragStart = m; dragValue = p.v; dragMoved = false; }
                return true;
            }
            case 5:
                if (left) b->onButton(h.idx);
                return true;
            default:
                if (left) { drag = Drag::Node; dragNode = b->id; dragOffset = {mc.x - g.box.position.x, mc.y - g.box.position.y}; }
                return true;
            }
        }

        if (const auto* kp = ev.getIf<sf::Event::KeyPressed>()) {
            if ((kp->code == sf::Keyboard::Key::Delete || kp->code == sf::Keyboard::Key::Backspace) && selected >= 0) {
                std::lock_guard<std::mutex> l(pipe.mtx);
                Block* b = pipe.find(selected);
                if (b && b->removable) { pipe.remove(selected); selected = -1; markDirty(); }
            }
            // Ctrl+F: search the blocks; Ctrl+0: zoom 100 %
            if (kp->control && kp->code == sf::Keyboard::Key::F) { searchFocus = true; search.clear(); }
            if (kp->control && (kp->code == sf::Keyboard::Key::Num0 || kp->code == sf::Keyboard::Key::Numpad0)) zoom = 1.0f;
            return true;   // the workspace has the keyboard while it is shown
        }
        if (ev.is<sf::Event::TextEntered>()) return true;
        return false;
    }

    // ---------------- drawing ----------------
    void draw(sf::RenderTarget& w, const sf::FloatRect& r, const std::string& rxInfo) {
        if (!font) return;
        ensureStore();
        lastDraw.restart();
        if (dirty && dirtyClock.getElapsedTime().asSeconds() > 0.8f) saveNow();
        if (choicesClock.getElapsedTime().asSeconds() > 0.5f) {
            choicesClock.restart();
            std::lock_guard<std::mutex> l(pipe.mtx);
            pipe.refreshChoices();
        }
        area = r;
        const Draw d = dr(w);
        const float TB = 34.0f, PW = 210.0f;
        toolbar = sf::FloatRect(r.position, {r.size.x, TB});
        palette = sf::FloatRect({r.position.x, r.position.y + TB}, {PW, r.size.y - TB});
        canvas = sf::FloatRect({r.position.x + PW, r.position.y + TB}, {r.size.x - PW, r.size.y - TB});
        mouseC = toCanvas(mouse);
        d.rect(r, sf::Color(17, 18, 22));

        // ---- canvas: grid, wires, blocks (a view: clipped to the canvas, zoomed) ----
        {
            const sf::View old = w.getView();
            const sf::Vector2f ws = old.getSize();
            sf::View v;
            v.setSize(canvas.size / zoom);
            v.setCenter(canvas.position + canvas.size / zoom / 2.0f);
            v.setViewport(sf::FloatRect({(canvas.position.x - (old.getCenter().x - ws.x / 2)) / ws.x, (canvas.position.y - (old.getCenter().y - ws.y / 2)) / ws.y},
                                        {canvas.size.x / ws.x, canvas.size.y / ws.y}));
            w.setView(v);
            const sf::FloatRect visible(canvas.position, canvas.size / zoom);
            // grid
            {
                sf::VertexArray dots(sf::PrimitiveType::Points);
                const float gs = 24.0f;
                const float ox = std::fmod(pan.x, gs), oy = std::fmod(pan.y, gs);
                for (float x = canvas.position.x + ox; x < visible.position.x + visible.size.x; x += gs)
                    for (float y = canvas.position.y + oy; y < visible.position.y + visible.size.y; y += gs)
                        dots.append({{x, y}, sf::Color(48, 50, 58)});
                w.draw(dots);
            }
            std::lock_guard<std::mutex> l(pipe.mtx);
            // wires: the colour of what flows (an output "like its input" shows the real kind)
            for (const Link& lk : pipe.links) {
                Block* a = pipe.find(lk.from); Block* b = pipe.find(lk.to);
                if (!a || !b) continue;
                const Geom ga = geom(*a), gb = geom(*b);
                if (lk.out >= (int)ga.outs.size() || lk.in >= (int)gb.ins.size()) continue;
                PT t = a->outs[lk.out].type;
                if (t == PT::Any && lk.out < (int)a->out.size() && a->out[lk.out].rate > 0) t = a->out[lk.out].type;
                bezier(w, ga.outs[lk.out], gb.ins[lk.in], ptColor(t));
            }
            if (drag == Drag::Link) {
                if (Block* a = pipe.find(dragNode)) {
                    const Geom ga = geom(*a);
                    if (dragOut < (int)ga.outs.size()) bezier(w, ga.outs[dragOut], mouseC, sf::Color(255, 255, 255, 160));
                }
            }
            const Hit hover = hitTest(mouseC);
            for (auto& bp : pipe.nodes) {
                Block& b = *bp;
                const Geom g = geom(b);
                if (!g.box.findIntersection(visible)) continue;
                const bool sel = b.id == selected;
                d.rect(g.box, sf::Color(30, 32, 38, 245), !b.warning.empty() ? sf::Color(230, 70, 70) : sel ? sf::Color(255, 160, 40) : sf::Color(64, 66, 76));
                const sf::Color cc = categoryColor(b.category);
                d.rect(g.head, sf::Color(cc.r / 2, cc.g / 2, cc.b / 2), sf::Color::Transparent);
                d.rect(sf::FloatRect(g.head.position, {4, HEAD}), cc);
                {
                    std::string t = b.title;
                    while (t.size() > 3 && measure(t, 12) > g.head.size.x - 34) t.pop_back();
                    d.text(t, g.head.position.x + 10, g.head.position.y + 4, 12, sf::Color::White);
                }
                if (b.removable) {
                    const bool hot = hover.node == b.id && hover.kind == 1 && mouseC.x > g.head.position.x + g.head.size.x - 20;
                    d.text("x", g.head.position.x + g.head.size.x - 14, g.head.position.y + 3, 12, hot ? sf::Color(255, 120, 120) : sf::Color(170, 170, 180));
                }
                float y = g.head.position.y + HEAD;
                if (g.hasStatus) {
                    std::string s = b.status();
                    while (!s.empty() && measure(s, 10) > g.box.size.x - 16) s.pop_back();
                    d.text(s, g.box.position.x + 8, y + 1, 10, sf::Color(150, 150, 162));
                    y += STATUS;
                }
                if (!b.warning.empty()) {
                    std::string s = "! " + b.warning;
                    while (!s.empty() && measure(s, 10) > g.box.size.x - 16) s.pop_back();
                    d.text(s, g.box.position.x + 8, y + 1, 10, sf::Color(255, 110, 110));
                    y += STATUS;
                }
                if (!b.note.empty()) {
                    std::string s = b.note;
                    while (!s.empty() && measure(s, 10) > g.box.size.x - 16) s.pop_back();
                    d.text(s, g.box.position.x + 8, y + 1, 10, sf::Color(240, 190, 90));
                }
                for (size_t i = 0; i < b.params.size(); i++) {
                    const Param& p = b.params[i];
                    const sf::FloatRect& pr = g.params[i];
                    if (pr.size.x <= 0) continue;   // hidden
                    const bool hot = hover.node == b.id && hover.kind == 4 && hover.idx == (int)i;
                    if (hot) d.rect(pr, sf::Color(50, 54, 66));
                    std::string lab = p.label;
                    const float labMax = pr.size.x * 0.55f;
                    while (lab.size() > 3 && measure(lab, 11) > labMax) lab.pop_back();
                    d.text(lab, pr.position.x + 2, pr.position.y + 1, 11, sf::Color(170, 170, 182));
                    std::string val = (editNode == b.id && editParam == (int)i) ? editBuf + "_" : p.show();
                    const sf::Color vc = (editNode == b.id && editParam == (int)i) ? sf::Color(255, 220, 120)
                                       : p.kind == Param::Choice ? sf::Color(255, 190, 110) : sf::Color(235, 235, 240);
                    const float maxW = pr.size.x - measure(lab, 11) - 12;
                    while (val.size() > 2 && measure(val, 11) > maxW) val.erase(val.size() - 2, 1);
                    d.text(val, pr.position.x + pr.size.x - measure(val, 11) - 2, pr.position.y + 1, 11, vc);
                }
                const auto bt = b.buttons();
                for (size_t i = 0; i < bt.size() && i < g.buttons.size(); i++)
                    button(d, g.buttons[i], bt[i], hover.node == b.id && hover.kind == 5 && hover.idx == (int)i);
                if (g.view.size.x > 0) b.drawView(d, g.view);
                for (size_t i = 0; i < g.ins.size(); i++) {
                    // growing inputs (Scope): each input dot in its trace's colour
                    const sf::Color cc = channelColor((int)i);
                    const bool chans = b.maxIns > 1 && b.ins.size() > 1 && pipe.linkInto(b.id, (int)i);
                    port(w, g.ins[i], b.ins[i].type, hover.node == b.id && hover.kind == 2 && hover.idx == (int)i, chans ? &cc : nullptr);
                    if (hover.node == b.id && hover.kind == 2 && hover.idx == (int)i) {
                        const std::string s = b.ins[i].name + " (" + ptName(b.ins[i].type) + ")";
                        d.rect(sf::FloatRect({g.ins[i].x - measure(s, 10) - 14, g.ins[i].y - 8}, {measure(s, 10) + 6, 15}), sf::Color(20, 20, 24, 230));
                        d.text(s, g.ins[i].x - measure(s, 10) - 11, g.ins[i].y - 7, 10, sf::Color(220, 220, 230));
                    }
                }
                for (size_t i = 0; i < g.outs.size(); i++) {
                    port(w, g.outs[i], b.outs[i].type, hover.node == b.id && hover.kind == 3 && hover.idx == (int)i);
                    if (hover.node == b.id && hover.kind == 3 && hover.idx == (int)i) {
                        const std::string s = b.outs[i].name + " (" + (b.outs[i].type == PT::Any ? std::string("same as the input") : ptName(b.outs[i].type)) + ")";
                        d.rect(sf::FloatRect({g.outs[i].x + 8, g.outs[i].y - 8}, {measure(s, 10) + 6, 15}), sf::Color(20, 20, 24, 230));
                        d.text(s, g.outs[i].x + 11, g.outs[i].y - 7, 10, sf::Color(220, 220, 230));
                    }
                }
            }
            w.setView(old);
            if (zoom != 1.0f) {
                char zb[32]; std::snprintf(zb, sizeof(zb), "%.0f %%", zoom * 100);
                d.text(zb, canvas.position.x + canvas.size.x - measure(zb, 11) - 10, canvas.position.y + 6, 11, sf::Color(140, 140, 150));
            }
        }

        // ---- palette: search, categories (click to fold), blocks (click or drag) ----
        d.rect(palette, sf::Color(24, 25, 30), sf::Color(50, 52, 60));
        paletteHits.clear();
        catHits.clear();
        {
            const float x = palette.position.x + 10;
            searchBox = sf::FloatRect({palette.position.x + 6, palette.position.y + 6}, {PW - 12, 22});
            d.rect(searchBox, searchFocus ? sf::Color(50, 44, 30) : sf::Color(34, 36, 44), searchFocus ? sf::Color(255, 160, 40) : sf::Color(70, 72, 84));
            if (search.empty() && !searchFocus) d.text("search blocks (Ctrl+F)", x, searchBox.position.y + 4, 11, sf::Color(110, 110, 122));
            else d.text(search + (searchFocus ? "_" : ""), x, searchBox.position.y + 4, 11, sf::Color(240, 240, 245));
            const auto items = paletteItems();
            const float top = palette.position.y + 34, bottom = palette.position.y + palette.size.y - 4;
            float y = top - paletteScroll;
            tipType.clear();
            if (!search.empty()) {
                // ranked matches: the title, then the category and the keyword that found it
                searchSel = std::clamp(searchSel, 0, std::max(0, (int)items.size() - 1));
                if (!items.empty()) d.text(std::to_string(items.size()) + (items.size() == 1 ? " match" : " matches") + "   (arrows + Enter)",
                                           x - 4, y, 10, sf::Color(130, 130, 142));
                y += 15;
                for (size_t i = 0; i < items.size(); i++) {
                    const BlockInfo* bi = items[i].bi;
                    const sf::FloatRect hr({palette.position.x + 4, y - 1}, {PW - 8, 28});
                    if (y >= top - 2 && y < bottom - 24) {
                        const bool sel = (int)i == searchSel;
                        if (sel || hr.contains(mouse)) d.rect(hr, sel ? sf::Color(58, 52, 36) : sf::Color(48, 52, 64));
                        d.rect(sf::FloatRect({hr.position.x, hr.position.y + 2}, {3, 24}), categoryColor(bi->category));
                        std::string t = bi->title;
                        while (t.size() > 3 && measure(t, 11) > PW - 22) t.pop_back();
                        d.text(t, x, y, 11, sf::Color(225, 225, 232));
                        std::string sub = bi->category;
                        if (!items[i].hint.empty()) sub += "  ~ " + items[i].hint;
                        while (sub.size() > 3 && measure(sub, 10) > PW - 22) sub.pop_back();
                        d.text(sub, x, y + 13, 10, sf::Color(140, 140, 152));
                        paletteHits.push_back({hr, bi->type});
                        if (hr.contains(mouse)) { tipType = bi->type; tipAt = {hr.position.x + hr.size.x + 8, hr.position.y}; }
                    }
                    y += 30;
                }
                const float content = y + paletteScroll - top;
                paletteScroll = std::clamp(paletteScroll, 0.0f, std::max(0.0f, content - (bottom - top) + 20));
                if (items.empty()) {
                    d.text("no block matches", x, top + 4, 11, sf::Color(150, 110, 110));
                    d.text("try a function: filter, clock,", x, top + 22, 10, sf::Color(120, 120, 132));
                    d.text("splot, szum, decymacja, crc ...", x, top + 36, 10, sf::Color(120, 120, 132));
                }
            }
            std::string cat;
            if (search.empty()) for (const auto& it : items) {
                const BlockInfo* bi = it.bi;
                if (bi->category != cat) {
                    cat = bi->category;
                    y += 3;
                    const bool fold = collapsed.count(cat) && search.empty();
                    const sf::FloatRect cr({palette.position.x + 4, y - 1}, {PW - 8, 16});
                    if (y >= top - 2 && y < bottom - 12) {
                        if (cr.contains(mouse)) d.rect(cr, sf::Color(40, 42, 52));
                        int n = 0; for (const auto& b2 : items) if (cat == b2.bi->category) n++;
                        d.text(std::string(fold ? "+ " : "- ") + cat + "  (" + std::to_string(n) + ")", x - 4, y, 11, categoryColor(cat));
                        catHits.push_back({cr, cat});
                    }
                    y += 16;
                }
                if (collapsed.count(cat) && search.empty()) continue;
                const sf::FloatRect hr({palette.position.x + 4, y - 1}, {PW - 8, 16});
                if (y >= top - 2 && y < bottom - 12) {
                    if (hr.contains(mouse)) d.rect(hr, sf::Color(48, 52, 64));
                    std::string t = bi->title;
                    while (t.size() > 3 && measure(t, 11) > PW - 30) t.pop_back();
                    d.text(t, x + 8, y, 11, sf::Color(220, 220, 228));
                    paletteHits.push_back({hr, bi->type});
                    if (hr.contains(mouse)) { tipType = bi->type; tipAt = {hr.position.x + hr.size.x + 8, hr.position.y}; }
                }
                y += 16;
            }
            // how far it may scroll
            if (search.empty()) {
                const float content = y + paletteScroll - top;
                paletteScroll = std::clamp(paletteScroll, 0.0f, std::max(0.0f, content - (bottom - top) + 20));
            }
        }

        // ---- toolbar ----
        d.rect(toolbar, sf::Color(30, 31, 37), sf::Color(55, 57, 66));
        toolHits.clear();
        {
            float x = toolbar.position.x + 8;
            const float y = toolbar.position.y + 6, h = 22;
            const std::string name = currentName();
            const std::string shown = editName ? editBuf + "_" : name + "  v";
            const float nw = std::clamp(measure(shown, 12) + 24, 180.0f, 320.0f);
            const sf::FloatRect nr({x, y}, {nw, h});
            d.rect(nr, editName ? sf::Color(60, 50, 20) : sf::Color(44, 46, 56), sf::Color(255, 150, 40));
            d.text(shown, x + 8, y + 4, 12, sf::Color::White);
            toolHits.push_back({nr, "chains"});
            x += nw + 10;
            const std::pair<const char*, const char*> bts[] = {{"new", "New"}, {"dup", "Duplicate"}, {"rename", "Rename"}, {"delete", "Delete"},
                                                               {"presets", "Presets v"}, {"import", "Import JSON"}, {"export", "Export JSON"}, {"exportall", "Export all"},
                                                               {"help", "?"}};
            for (const auto& b : bts) {
                const float bw = measure(b.second, 11) + 18;
                const sf::FloatRect br({x, y}, {bw, h});
                button(d, br, b.second, br.contains(mouse), (menu == 2 && std::string(b.first) == "presets") || (showHelp && std::string(b.first) == "help"));
                toolHits.push_back({br, b.first});
                x += bw + 6;
            }
            std::string right = rxInfo;
            if (msgClock.getElapsedTime().asSeconds() < 6.0f && !msg.empty()) right = msg;
            else if (dirty) right += "   (saving...)";
            const float maxRight = toolbar.position.x + toolbar.size.x - x - 20;
            while (right.size() > 3 && measure(right, 11) > maxRight) right.erase(0, 1);
            d.text(right, toolbar.position.x + toolbar.size.x - measure(right, 11) - 12, y + 4, 11,
                   msgClock.getElapsedTime().asSeconds() < 6.0f && !msg.empty() ? sf::Color(255, 200, 120) : sf::Color(150, 150, 162));
        }

        // ---- help ----
        if (showHelp) {
            const char* help[] = {
                "Blocks: drag from the list onto the canvas (or click: next to the last one). Search (Ctrl+F): name or function, arrows + Enter.",
                "Wires: drag from an output (right side) to an input (left side). Drag a connected input to pull its wire off;",
                "       right-click an input to disconnect. Colours: I/Q blue, real green, symbols/bits orange, frames pink, image white.",
                "       An output marked 'same as the input' carries what comes in (I/Q or real).",
                "       Scope: every wire opens one more input (up to 8), each channel in its own colour.",
                "       Feedback: a wire may go back to an earlier block if the loop passes a Delay (z^-n); n samples per step.",
                "Numbers: drag sideways, mouse wheel (Shift: finer), or click and type. Choices: click for the list, right-click: previous.",
                "Canvas: drag the empty space to move, wheel to scroll, Ctrl+wheel to zoom (Ctrl+0: 100 %). Del: remove the selected block.",
                "Signal: the Channel input takes a receiver (its orange Analyzer channel; the Analyzer goes on by itself). More inputs:",
                "       Sources > Channel input. Sources (generators) run on their own, so a chain works without a radio.",
                "       Add / Multiply / Real to complex line up their inputs; one at another rate is converted (amber note on the block).",
                "Tabs: '+ Pipeline' opens another chain (each runs on its own thread); drag a tab sideways to reorder.",
                "Saved: every change, a moment later (pipelines.json). Export / Import JSON to share chains.",
            };
            float hh = 12.0f + 16.0f * (sizeof(help) / sizeof(help[0]));
            const sf::FloatRect hr({canvas.position.x + 20, canvas.position.y + 14}, {std::min(canvas.size.x - 40, 820.0f), hh});
            d.rect(hr, sf::Color(26, 28, 36, 245), sf::Color(255, 150, 40));
            float yy = hr.position.y + 6;
            for (const char* hl : help) { d.text(hl, hr.position.x + 10, yy, 11, sf::Color(215, 215, 225)); yy += 16; }
        }

        // ---- keywords of the palette block under the mouse ----
        if (!tipType.empty() && drag == Drag::None && menu == 0) {
            const std::string tags = blockTags(tipType);
            std::vector<std::string> lines;
            std::string line;
            const float maxW = 300.0f;
            std::istringstream ts(tags);
            for (std::string w; ts >> w;) {
                const std::string next = line.empty() ? w : line + ", " + w;
                if (!line.empty() && measure(next, 11) > maxW) { lines.push_back(line + ","); line = w; }
                else line = next;
            }
            if (!line.empty()) lines.push_back(line);
            if (!lines.empty()) {
                float w = measure("keywords:", 10);
                for (const auto& l : lines) w = std::max(w, measure(l, 11));
                sf::FloatRect tr({tipAt.x, tipAt.y}, {w + 16, 22.0f + 15.0f * lines.size()});
                tr.position.y = std::min(tr.position.y, r.position.y + r.size.y - tr.size.y - 4);
                d.rect(tr, sf::Color(26, 28, 36, 240), sf::Color(90, 92, 106));
                d.text("keywords:", tr.position.x + 8, tr.position.y + 4, 10, sf::Color(140, 140, 152));
                float yy = tr.position.y + 18;
                for (const auto& l : lines) { d.text(l, tr.position.x + 8, yy, 11, sf::Color(205, 205, 215)); yy += 15; }
            }
        }

        // ---- a block being dragged in from the palette ----
        if (drag == Drag::Palette && dragMoved) {
            std::string t = dragType, cat;
            for (const auto& bi : blockList()) if (dragType == bi.type) { t = bi.title; cat = bi.category; }
            const sf::FloatRect gr({mouse.x - 30.0f * zoom, mouse.y - 10.0f * zoom}, {210.0f * zoom, HEAD * zoom});
            const sf::Color cc = categoryColor(cat);
            d.rect(gr, sf::Color(cc.r / 2, cc.g / 2, cc.b / 2, 220), canvas.contains(mouse) ? sf::Color(255, 160, 40) : sf::Color(120, 120, 130));
            d.text(t, gr.position.x + 10, gr.position.y + 4, 12, sf::Color::White);
        }

        // ---- menus ----
        menuHits.clear();
        if (menu) {
            std::vector<std::pair<std::string, int>> items;
            std::string current;
            float x = toolbar.position.x + 8, y0 = toolbar.position.y + toolbar.size.y;
            if (menu == 1) {
                for (size_t i = 0; i < store().items.size(); i++) items.push_back({store().items[i].name, (int)i});
                items.push_back({"+ New chain", -1});
                current = currentName();
            } else if (menu == 2) {
                for (size_t i = 0; i < presets().size(); i++) items.push_back({presets()[i].name, (int)i});
                for (const auto& b : toolHits) if (b.id == "presets") x = b.r.position.x;
            } else {
                std::lock_guard<std::mutex> l(pipe.mtx);
                if (Block* b = pipe.find(menuNode)) if (menuParam < (int)b->params.size()) {
                    const Param& p = b->params[menuParam];
                    for (size_t i = 0; i < p.choices.size(); i++) items.push_back({p.choices[i], (int)i});
                    current = p.show();
                }
                x = menuAt.x; y0 = menuAt.y;
            }
            float mw = 220;
            for (const auto& it : items) mw = std::max(mw, measure(it.first, 12) + 30);
            const float mh = items.size() * 20.0f + 6;
            if (y0 + mh > area.position.y + area.size.y) y0 = std::max(area.position.y, area.position.y + area.size.y - mh);
            if (x + mw > area.position.x + area.size.x) x = area.position.x + area.size.x - mw;
            d.rect(sf::FloatRect({x, y0}, {mw, mh}), sf::Color(36, 38, 46), sf::Color(255, 150, 40));
            float y = y0 + 3;
            for (const auto& it : items) {
                const sf::FloatRect ir({x + 2, y}, {mw - 4, 20});
                if (ir.contains(mouse)) d.rect(ir, sf::Color(70, 60, 40));
                d.text(it.first, x + 12, y + 3, 12, it.first == current ? sf::Color(255, 190, 110) : sf::Color(225, 225, 232));
                menuHits.push_back({ir, it.second});
                y += 20;
            }
        }
    }

    // True when the workspace was drawn lately (its tab is shown)
    bool visible() const { return lastDraw.getElapsedTime().asSeconds() < 1.0f; }
};

} // namespace pl
