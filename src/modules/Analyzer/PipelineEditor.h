#pragma once

#include "Pipeline.h"
#include <SFML/Graphics.hpp>
#include <functional>
#include <string>
#include <vector>
#include <cmath>

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

// Set once by main.cpp (native dialogs, settings folder)
struct EditorHooks {
    std::function<std::string()> storePath;                                            // pipelines.json
    std::function<std::string(const std::string& name)> saveJsonDialog;                // empty = cancelled
    std::function<std::string()> openJsonDialog;
    std::function<void(const std::string& title, const std::string& msg)> popup;
};
inline EditorHooks& hooks() { static EditorHooks h; return h; }

class PipelineEditor {
    Pipeline& pipe;
    const sf::Font* font = nullptr;
    sf::FloatRect area;               // the whole workspace (window coordinates)
    sf::FloatRect canvas, palette, toolbar;
    sf::Vector2f pan {0.0f, 0.0f};
    int selected = -1;

    enum class Drag { None, Node, Pan, Link, Value };
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
        g.hasStatus = !b.status().empty() || b.type == "input";
        if (g.hasStatus) cy += STATUS;
        for (size_t i = 0; i < b.params.size(); i++) { g.params.push_back(sf::FloatRect({x + 8, cy}, {w - 16, ROW})); cy += ROW; }
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

    float measure(const std::string& s, unsigned size) const {
        if (!font) return 0.0f;
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size * 2);
        return t.getGlobalBounds().size.x * 0.5f;
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
        if (dirty) saveNow();
        if (!loadJson(store().items[i].json, name)) say("\"" + name + "\" could not be read");
    }

    std::string currentName() { std::lock_guard<std::mutex> l(pipe.mtx); return pipe.name; }

    void toolbarAction(const std::string& id) {
        auto& st = store();
        if (id == "chains") { menu = menu == 1 ? 0 : 1; return; }
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
            if (st.items.empty()) {
                { std::lock_guard<std::mutex> l(pipe.mtx); pipe.clear(); pipe.name = "New chain"; }
                saveNow();
            } else {
                loadJson(st.items[0].json, st.items[0].name);
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

    void port(sf::RenderTarget& w, sf::Vector2f p, PT t, bool hot) const {
        sf::CircleShape c(hot ? 6.0f : 5.0f);
        c.setOrigin({c.getRadius(), c.getRadius()});
        c.setPosition(p);
        c.setFillColor(ptColor(t));
        c.setOutlineColor(sf::Color(20, 20, 24));
        c.setOutlineThickness(1.5f);
        w.draw(c);
    }

    void button(const Draw& d, const sf::FloatRect& r, const std::string& s, bool hot, bool active = false) const {
        d.rect(r, active ? sf::Color(120, 70, 10) : hot ? sf::Color(60, 64, 76) : sf::Color(40, 42, 50), sf::Color(80, 80, 92));
        d.text(s, r.position.x + (r.size.x - measure(s, 11)) / 2, r.position.y + (r.size.y - 14) / 2, 11, sf::Color(225, 225, 232));
    }

public:
    explicit PipelineEditor(Pipeline& p) : pipe(p) {}
    void setFont(const sf::Font* f) { font = f; }
    std::string title() { return currentName(); }

    // Called when the tab is shown (loads the saved chains on first use)
    void open() { ensureStore(); }

    // ---------------- events (window coordinates) ----------------
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        if (!font) return false;
        ensureStore();
        auto toWin = [&](sf::Vector2i p) { return win.mapPixelToCoords(p); };

        // typing
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
            std::lock_guard<std::mutex> l(pipe.mtx);
            if (drag == Drag::Node) {
                if (Block* b = pipe.find(dragNode)) { b->x = mouse.x - origin().x - dragOffset.x; b->y = mouse.y - origin().y - dragOffset.y; markDirty(); }
            } else if (drag == Drag::Pan) {
                pan += mouse - dragStart; dragStart = mouse;
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
            if (drag == Drag::Link) {
                std::lock_guard<std::mutex> l(pipe.mtx);
                const Hit h = hitTest(mouse);
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
            if (canvas.contains(m)) {
                std::lock_guard<std::mutex> l(pipe.mtx);
                const Hit h = hitTest(m);
                if (h.kind == 4) {
                    if (Block* b = pipe.find(h.node)) {
                        const bool fine = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift);
                        const bool coarse = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl);
                        stepParam(b, h.idx, (sc->delta > 0 ? 1.0 : -1.0) * (coarse ? 10.0 : fine ? 0.1 : 1.0));
                    }
                } else {
                    pan.y += sc->delta * 40.0f;   // scroll the canvas
                }
            }
            return true;
        }

        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            const sf::Vector2f m = toWin(mb->position);
            mouse = m;
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
                            loadJson(pr.json, n);
                            saveNow();
                            say("Preset loaded as \"" + n + "\"");
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
                for (const auto& p : paletteHits) if (p.first.contains(m) && left) { addBlock(p.second); return true; }
                return true;
            }
            if (!canvas.contains(m)) return true;
            std::lock_guard<std::mutex> l(pipe.mtx);
            const Hit h = hitTest(m);
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
                    if (b->removable && m.x > g.head.position.x + g.head.size.x - 20) { pipe.remove(b->id); selected = -1; markDirty(); return true; }
                    drag = Drag::Node; dragNode = b->id; dragOffset = {m.x - g.box.position.x, m.y - g.box.position.y};
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
                if (p.kind == Param::Choice) { stepParam(b, h.idx, right ? -1 : 1); return true; }
                if (p.kind == Param::Text) { if (left) { editNode = b->id; editParam = h.idx; editBuf = p.text; } return true; }
                if (left) { drag = Drag::Value; dragNode = b->id; dragParam = h.idx; dragStart = m; dragValue = p.v; dragMoved = false; }
                return true;
            }
            case 5:
                if (left) b->onButton(h.idx);
                return true;
            default:
                if (left) { drag = Drag::Node; dragNode = b->id; dragOffset = {m.x - g.box.position.x, m.y - g.box.position.y}; }
                return true;
            }
        }

        if (const auto* kp = ev.getIf<sf::Event::KeyPressed>()) {
            if ((kp->code == sf::Keyboard::Key::Delete || kp->code == sf::Keyboard::Key::Backspace) && selected >= 0) {
                std::lock_guard<std::mutex> l(pipe.mtx);
                Block* b = pipe.find(selected);
                if (b && b->removable) { pipe.remove(selected); selected = -1; markDirty(); }
            }
            return true;   // the workspace has the keyboard while it is shown
        }
        if (ev.is<sf::Event::TextEntered>()) return true;
        return false;
    }

    // ---------------- drawing ----------------
    void draw(sf::RenderTarget& w, const sf::FloatRect& r, const std::string& rxInfo) {
        if (!font) return;
        ensureStore();
        if (dirty && dirtyClock.getElapsedTime().asSeconds() > 0.8f) saveNow();
        area = r;
        const Draw d = dr(w);
        const float TB = 34.0f, PW = 190.0f;
        toolbar = sf::FloatRect(r.position, {r.size.x, TB});
        palette = sf::FloatRect({r.position.x, r.position.y + TB}, {PW, r.size.y - TB});
        canvas = sf::FloatRect({r.position.x + PW, r.position.y + TB}, {r.size.x - PW, r.size.y - TB});
        d.rect(r, sf::Color(17, 18, 22));

        // ---- canvas: grid, wires, blocks ----
        {
            const sf::View old = w.getView();
            // clip to the canvas (a view on its part of the window)
            const sf::Vector2f ws = old.getSize();
            sf::View v(sf::FloatRect(canvas.position - (old.getCenter() - ws / 2.0f), canvas.size));
            v.setCenter(canvas.position + canvas.size / 2.0f);
            v.setSize(canvas.size);
            v.setViewport(sf::FloatRect({(canvas.position.x - (old.getCenter().x - ws.x / 2)) / ws.x, (canvas.position.y - (old.getCenter().y - ws.y / 2)) / ws.y},
                                        {canvas.size.x / ws.x, canvas.size.y / ws.y}));
            w.setView(v);
            // grid
            {
                sf::VertexArray dots(sf::PrimitiveType::Points);
                const float gs = 24.0f;
                const float ox = std::fmod(pan.x, gs), oy = std::fmod(pan.y, gs);
                for (float x = canvas.position.x + ox; x < canvas.position.x + canvas.size.x; x += gs)
                    for (float y = canvas.position.y + oy; y < canvas.position.y + canvas.size.y; y += gs)
                        dots.append({{x, y}, sf::Color(48, 50, 58)});
                w.draw(dots);
            }
            std::lock_guard<std::mutex> l(pipe.mtx);
            // wires
            for (const Link& lk : pipe.links) {
                Block* a = pipe.find(lk.from); Block* b = pipe.find(lk.to);
                if (!a || !b) continue;
                const Geom ga = geom(*a), gb = geom(*b);
                if (lk.out >= (int)ga.outs.size() || lk.in >= (int)gb.ins.size()) continue;
                bezier(w, ga.outs[lk.out], gb.ins[lk.in], ptColor(a->outs[lk.out].type));
            }
            if (drag == Drag::Link) {
                if (Block* a = pipe.find(dragNode)) {
                    const Geom ga = geom(*a);
                    if (dragOut < (int)ga.outs.size()) bezier(w, ga.outs[dragOut], mouse, sf::Color(255, 255, 255, 160));
                }
            }
            const Hit hover = hitTest(mouse);
            for (auto& bp : pipe.nodes) {
                Block& b = *bp;
                const Geom g = geom(b);
                if (!g.box.findIntersection(canvas)) continue;
                const bool sel = b.id == selected;
                d.rect(g.box, sf::Color(30, 32, 38, 245), sel ? sf::Color(255, 160, 40) : sf::Color(64, 66, 76));
                const sf::Color cc = categoryColor(b.category);
                d.rect(g.head, sf::Color(cc.r / 2, cc.g / 2, cc.b / 2), sf::Color::Transparent);
                d.rect(sf::FloatRect(g.head.position, {4, HEAD}), cc);
                d.text(b.title, g.head.position.x + 10, g.head.position.y + 4, 12, sf::Color::White);
                if (b.removable) {
                    const bool hot = hover.node == b.id && hover.kind == 1 && mouse.x > g.head.position.x + g.head.size.x - 20;
                    d.text("x", g.head.position.x + g.head.size.x - 14, g.head.position.y + 3, 12, hot ? sf::Color(255, 120, 120) : sf::Color(170, 170, 180));
                }
                float y = g.head.position.y + HEAD;
                if (g.hasStatus) {
                    std::string s = b.status();
                    while (!s.empty() && measure(s, 10) > g.box.size.x - 16) s.pop_back();
                    d.text(s, g.box.position.x + 8, y + 1, 10, sf::Color(150, 150, 162));
                }
                for (size_t i = 0; i < b.params.size(); i++) {
                    const Param& p = b.params[i];
                    const sf::FloatRect& pr = g.params[i];
                    const bool hot = hover.node == b.id && hover.kind == 4 && hover.idx == (int)i;
                    if (hot) d.rect(pr, sf::Color(50, 54, 66));
                    d.text(p.label, pr.position.x + 2, pr.position.y + 1, 11, sf::Color(170, 170, 182));
                    std::string val = (editNode == b.id && editParam == (int)i) ? editBuf + "_" : p.show();
                    const sf::Color vc = (editNode == b.id && editParam == (int)i) ? sf::Color(255, 220, 120)
                                       : p.kind == Param::Choice ? sf::Color(255, 190, 110) : sf::Color(235, 235, 240);
                    const float maxW = pr.size.x * 0.55f;
                    while (val.size() > 2 && measure(val, 11) > maxW) val.erase(val.size() - 2, 1);
                    d.text(val, pr.position.x + pr.size.x - measure(val, 11) - 2, pr.position.y + 1, 11, vc);
                }
                const auto bt = b.buttons();
                for (size_t i = 0; i < bt.size() && i < g.buttons.size(); i++)
                    button(d, g.buttons[i], bt[i], hover.node == b.id && hover.kind == 5 && hover.idx == (int)i);
                if (g.view.size.x > 0) b.drawView(d, g.view);
                for (size_t i = 0; i < g.ins.size(); i++) {
                    port(w, g.ins[i], b.ins[i].type, hover.node == b.id && hover.kind == 2 && hover.idx == (int)i);
                    if (hover.node == b.id && hover.kind == 2 && hover.idx == (int)i)
                        d.text(b.ins[i].name + " (" + ptName(b.ins[i].type) + ")", g.ins[i].x - measure(b.ins[i].name + " (" + ptName(b.ins[i].type) + ")", 10) - 10, g.ins[i].y - 7, 10, sf::Color(220, 220, 230));
                }
                for (size_t i = 0; i < g.outs.size(); i++) {
                    port(w, g.outs[i], b.outs[i].type, hover.node == b.id && hover.kind == 3 && hover.idx == (int)i);
                    if (hover.node == b.id && hover.kind == 3 && hover.idx == (int)i)
                        d.text(b.outs[i].name + " (" + ptName(b.outs[i].type) + ")", g.outs[i].x + 10, g.outs[i].y - 7, 10, sf::Color(220, 220, 230));
                }
            }
            w.setView(old);
        }

        // ---- palette ----
        d.rect(palette, sf::Color(24, 25, 30), sf::Color(50, 52, 60));
        paletteHits.clear();
        {
            float y = palette.position.y + 8;
            const float x = palette.position.x + 10;
            d.text("Blocks (click to add)", x, y, 12, sf::Color(255, 160, 40)); y += 20;
            std::string cat;
            for (const auto& bi : blockList()) {
                if (bi.category != cat) {
                    cat = bi.category;
                    y += 4;
                    d.text(cat, x, y, 10, categoryColor(cat)); y += 14;
                }
                const sf::FloatRect hr({palette.position.x + 4, y - 1}, {PW - 8, 16});
                if (hr.contains(mouse)) d.rect(hr, sf::Color(48, 52, 64));
                d.text(bi.title, x + 8, y, 11, sf::Color(220, 220, 228));
                paletteHits.push_back({hr, bi.type});
                y += 16;
            }
            y += 12;
            const char* help[] = {"Wire: drag from an output (right)", "  to an input (left).", "Pull a wire off: drag its input.",
                                  "Right-click an input: disconnect.", "Numbers: drag sideways, wheel", "  (Shift fine, Ctrl x10), or click",
                                  "  and type. Choices: click.", "Del: remove the selected block.", "Drag the empty canvas: move.",
                                  "", "The channel input is the orange", "Analyzer channel of this receiver."};
            for (const char* hl : help) { if (y > palette.position.y + palette.size.y - 14) break; d.text(hl, x, y, 10, sf::Color(130, 130, 142)); y += 13; }
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
                                                               {"presets", "Presets v"}, {"import", "Import JSON"}, {"export", "Export JSON"}, {"exportall", "Export all"}};
            for (const auto& b : bts) {
                const float bw = measure(b.second, 11) + 18;
                const sf::FloatRect br({x, y}, {bw, h});
                button(d, br, b.second, br.contains(mouse), (menu == 2 && std::string(b.first) == "presets"));
                toolHits.push_back({br, b.first});
                x += bw + 6;
            }
            std::string right = rxInfo;
            if (msgClock.getElapsedTime().asSeconds() < 6.0f && !msg.empty()) right = msg;
            else if (dirty) right += "   (saving...)";
            d.text(right, toolbar.position.x + toolbar.size.x - measure(right, 11) - 12, y + 4, 11,
                   msgClock.getElapsedTime().asSeconds() < 6.0f && !msg.empty() ? sf::Color(255, 200, 120) : sf::Color(150, 150, 162));
        }

        // ---- menus ----
        menuHits.clear();
        if (menu) {
            std::vector<std::pair<std::string, int>> items;
            if (menu == 1) {
                for (size_t i = 0; i < store().items.size(); i++) items.push_back({store().items[i].name, (int)i});
                items.push_back({"+ New chain", -1});
            } else {
                for (size_t i = 0; i < presets().size(); i++) items.push_back({presets()[i].name, (int)i});
            }
            float x = toolbar.position.x + 8;
            if (menu == 2) for (const auto& b : toolHits) if (b.id == "presets") x = b.r.position.x;
            const float y0 = toolbar.position.y + toolbar.size.y;
            float mw = 220;
            for (const auto& it : items) mw = std::max(mw, measure(it.first, 12) + 30);
            d.rect(sf::FloatRect({x, y0}, {mw, items.size() * 20.0f + 6}), sf::Color(36, 38, 46), sf::Color(255, 150, 40));
            float y = y0 + 3;
            const std::string cur = currentName();
            for (const auto& it : items) {
                const sf::FloatRect ir({x + 2, y}, {mw - 4, 20});
                if (ir.contains(mouse)) d.rect(ir, sf::Color(70, 60, 40));
                d.text(it.first, x + 12, y + 3, 12, menu == 1 && it.first == cur ? sf::Color(255, 190, 110) : sf::Color(225, 225, 232));
                menuHits.push_back({ir, it.second});
                y += 20;
            }
        }
    }
};

} // namespace pl
