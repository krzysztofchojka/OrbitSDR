#pragma once
// =====================================================================
//  Packets and telemetry (included from Pipeline.h)
//
//  "Packets by a length field": bits after a Frame sync -> one frame per
//  packet, as long as its length byte says (CC1101, SX127x / RFM69 in FSK
//  mode, Si4463, nRF905 ... and many sensors and radiosondes).
//  "Telemetry fields": numbers out of the bytes of each frame (integers
//  of any size and byte order, floats, bit fields, text), scaled into
//  units, shown as a table and given on as text lines.
// =====================================================================

class LengthPackets : public Block {
    std::vector<uint8_t> cur;
    int nbits = 0, skipLeft = 0;
    uint8_t acc = 0;
    bool in_ = false;
    long long packets = 0, tooLong = 0;
    long want = -1;   // bytes of this packet (-1: not known yet)
public:
    LengthPackets() {
        type = "lenpkt"; title = "Packets by a length field"; category = "Frames"; ins = {{"bits (after Frame sync)", PT::Sym}}; setOuts({{"frames", PT::Frames}});
        addNum("skip", "Leave out after a frame start", 32, 0, 1024, 8, "bits");
        addChoice("o", "Bit order", {"MSB first", "LSB first"});
        addNum("lpos", "Length field at byte", 0, 0, 4096, 1, "");
        addChoice("lsz", "Length field", {"1 byte", "2 bytes, big-endian", "2 bytes, little-endian", "low 7 bits of a byte", "low 6 bits of a byte"});
        addNum("add", "Bytes after the field = length +", 0, -1024, 1024, 1, "");
        addChoice("inc", "Frame from", {"the first byte (with the length)", "after the length field"});
        addNum("max", "Longest packet", 255, 1, 65535, 1, "bytes");
    }
    std::string status() const override { return std::to_string(packets) + " packets" + (tooLong ? ", " + std::to_string(tooLong) + " too long" : std::string()); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate / 8;
        const bool lsb = choice("o") == 1;
        const size_t lpos = (size_t)num("lpos");
        const int lsz = choice("lsz");
        const size_t fieldLen = (lsz == 1 || lsz == 2) ? 2 : 1;
        auto emit = [&]() {
            std::vector<uint8_t> f = cur;
            if (choice("inc") == 1) f.erase(f.begin(), f.begin() + (long)std::min(f.size(), lpos + fieldLen));
            o.frames.push_back(std::move(f));
            packets++;
            in_ = false; cur.clear();
        };
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { cur.clear(); in_ = true; nbits = 0; acc = 0; want = -1; skipLeft = (int)num("skip"); continue; }
            if (!in_) continue;
            if (skipLeft > 0) { skipLeft--; continue; }
            acc = lsb ? (uint8_t)((acc >> 1) | ((b & 1) << 7)) : (uint8_t)((acc << 1) | (b & 1));
            if (++nbits < 8) continue;
            cur.push_back(acc); nbits = 0; acc = 0;
            if (want < 0 && cur.size() == lpos + fieldLen) {
                long L = 0;
                switch (lsz) {
                case 1: L = cur[lpos] << 8 | cur[lpos + 1]; break;
                case 2: L = cur[lpos] | cur[lpos + 1] << 8; break;
                case 3: L = cur[lpos] & 0x7F; break;
                case 4: L = cur[lpos] & 0x3F; break;
                default: L = cur[lpos];
                }
                want = (long)(lpos + fieldLen) + L + (long)num("add");
                if (want <= (long)(lpos + fieldLen) - 1 || want > (long)num("max") + (long)(lpos + fieldLen)) { tooLong++; in_ = false; cur.clear(); continue; }
            }
            if (want >= 0 && (long)cur.size() >= want) emit();
        }
    }
};

// One field of a frame: "name type @offset [*scale] [+offset] [unit]"
struct Field {
    std::string name, type, unit;
    size_t off = 0;
    int bit = 0, nbits = 0, count = 0;  // bit fields: @byte.bit:n (bit 0 = the byte's MSB); char / hex: count
    double scale = 1.0, add = 0.0;
    bool scaled = false;
    double lo = 1e300, hi = -1e300, last = 0.0;
    std::string lastText;
    bool seen = false;
};

inline std::vector<Field> parseFields(const std::string& spec, std::string& err) {
    std::vector<Field> v;
    std::string item;
    std::stringstream ss(spec);
    while (std::getline(ss, item, ';')) {
        std::stringstream is(item);
        std::vector<std::string> w;
        std::string t;
        while (is >> t) w.push_back(t);
        if (w.empty()) continue;
        if (w.size() < 3) { err = "\"" + item + "\": needs name type @offset"; continue; }
        Field f;
        f.name = w[0];
        f.type = w[1];
        for (char& c : f.type) c = (char)std::tolower((unsigned char)c);
        size_t i = 2;
        if (f.type == "char" || f.type == "hex") {
            if (w[i][0] == '@') { err = f.name + ": char / hex need a count (char 6 @2)"; continue; }
            f.count = std::max(1, std::atoi(w[i].c_str())); i++;
        }
        if (i >= w.size() || w[i][0] != '@') { err = f.name + ": the offset (@n) is missing"; continue; }
        const std::string at = w[i].substr(1);
        f.off = (size_t)std::strtoul(at.c_str(), nullptr, 0);
        if (f.type == "bits") {
            const size_t dot = at.find('.'), col = at.find(':');
            f.bit = dot != std::string::npos ? std::atoi(at.c_str() + dot + 1) : 0;
            f.nbits = col != std::string::npos ? std::atoi(at.c_str() + col + 1) : 1;
            f.nbits = std::clamp(f.nbits, 1, 32);
        }
        i++;
        for (; i < w.size(); i++) {
            if (w[i][0] == '*' || w[i][0] == '/') { const double k = std::atof(w[i].c_str() + 1); f.scale = w[i][0] == '*' ? k : (k != 0 ? 1.0 / k : 1.0); f.scaled = true; }
            else if ((w[i][0] == '+' || w[i][0] == '-') && w[i].size() > 1 && (std::isdigit((unsigned char)w[i][1]) || w[i][1] == '.')) { f.add = std::atof(w[i].c_str()); f.scaled = true; }
            else f.unit += (f.unit.empty() ? "" : " ") + w[i];
        }
        static const std::set<std::string> known = {"u8", "i8", "u16le", "u16be", "i16le", "i16be", "u24le", "u24be", "i24le", "i24be",
                                                    "u32le", "u32be", "i32le", "i32be", "f32le", "f32be", "f64le", "f64be", "bits", "char", "hex"};
        if (!known.count(f.type)) { err = f.name + ": unknown type " + f.type; continue; }
        v.push_back(f);
    }
    return v;
}

// The value of a field in frame f; false when the frame is too short
inline bool fieldValue(const Field& d, const std::vector<uint8_t>& f, double& val, std::string& txt) {
    const std::string& t = d.type;
    if (t == "char" || t == "hex") {
        if (d.off + d.count > f.size()) return false;
        txt.clear();
        for (int k = 0; k < d.count; k++) {
            const uint8_t c = f[d.off + k];
            if (t == "char") txt += (c >= 32 && c < 127) ? (char)c : '.';
            else { char b[4]; std::snprintf(b, sizeof(b), "%02X", c); txt += b; }
        }
        return true;
    }
    if (t == "bits") {
        const size_t first = d.off * 8 + (size_t)d.bit, lastBit = first + (size_t)d.nbits;
        if ((lastBit + 7) / 8 > f.size()) return false;
        uint64_t v = 0;
        for (size_t b = first; b < lastBit; b++) v = (v << 1) | ((f[b / 8] >> (7 - b % 8)) & 1);
        val = (double)v;
        return true;
    }
    const int bytes = (t[1] == '8') ? 1 : (t.compare(1, 2, "16") == 0) ? 2 : (t.compare(1, 2, "24") == 0) ? 3 : (t.compare(1, 2, "32") == 0) ? 4 : 8;
    if (d.off + bytes > f.size()) return false;
    const bool le = t.size() > 3 && t.compare(t.size() - 2, 2, "le") == 0;
    uint64_t u = 0;
    for (int k = 0; k < bytes; k++) {
        const uint8_t c = f[d.off + (le ? bytes - 1 - k : k)];
        u = (u << 8) | c;
    }
    if (t[0] == 'u') val = (double)u;
    else if (t[0] == 'i') { const int sh = 64 - 8 * bytes; val = (double)((int64_t)(u << sh) >> sh); }
    else if (bytes == 4) { uint32_t x = (uint32_t)u; float fl; std::memcpy(&fl, &x, 4); val = fl; }
    else { double db; std::memcpy(&db, &u, 8); val = db; }
    return true;
}

class FieldsBlock : public Block {
    std::vector<Field> fields;
    std::string err;
    long long frames = 0, shortOnes = 0;
    static std::string fmt(double v) {
        char b[40];
        if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(b, sizeof(b), "%.0f", v);
        else std::snprintf(b, sizeof(b), "%.6g", v);
        return b;
    }
public:
    FieldsBlock() {
        type = "fields"; title = "Telemetry fields (decode a frame)"; category = "Frames"; ins = {{"frames", PT::Frames}}; setOuts({{"text lines", PT::Frames}});
        addText("f", "Fields (name type @byte [*k] [+c] [unit]; ...)", "id u8 @0; temp i16le @1 *0.01 C; volt u16le @3 *0.001 V; flags bits @5.0:4");
        addNum("min", "Shortest frame", 1, 0, 65536, 1, "bytes");
    }
    std::vector<std::string> buttons() const override { return {"Reset min / max"}; }
    void onButton(int) override { for (auto& f : fields) { f.lo = 1e300; f.hi = -1e300; } }
    std::string status() const override {
        if (!err.empty()) return "! " + err;
        return std::to_string(frames) + " frames" + (shortOnes ? ", " + std::to_string(shortOnes) + " too short" : std::string());
    }
    sf::Vector2f viewSize() const override { return {420.0f, 26.0f + 13.0f * std::clamp((float)fields.size(), 3.0f, 16.0f)}; }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        float y = r.position.y + 3;
        const float x0 = r.position.x + 6, x1 = r.position.x + 120, x2 = r.position.x + 260, x3 = r.position.x + 340;
        d.text("field", x0, y, 10, sf::Color(130, 130, 140)); d.text("value", x1, y, 10, sf::Color(130, 130, 140));
        d.text("min", x2, y, 10, sf::Color(130, 130, 140)); d.text("max", x3, y, 10, sf::Color(130, 130, 140));
        y += 15;
        for (const Field& f : fields) {
            if (y > r.position.y + r.size.y - 12) break;
            d.text(f.name, x0, y, 10, sf::Color(200, 200, 210));
            std::string v = !f.seen ? "-" : (f.type == "char" || f.type == "hex") ? f.lastText : fmt(f.last) + (f.unit.empty() ? "" : " " + f.unit);
            d.text(v, x1, y, 10, sf::Color(140, 255, 160));
            if (f.seen && f.type != "char" && f.type != "hex" && f.lo <= f.hi) {
                d.text(fmt(f.lo), x2, y, 10, sf::Color(150, 170, 200));
                d.text(fmt(f.hi), x3, y, 10, sf::Color(255, 180, 120));
            }
            y += 13;
        }
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (changed(1.0)) { err.clear(); fields = parseFields(text("f"), err); }
        if (!in[0]) return;
        o.rate = in[0]->rate;
        for (const auto& fr : in[0]->frames) {
            if (fr.size() < (size_t)num("min")) { shortOnes++; continue; }
            frames++;
            std::string line;
            for (Field& f : fields) {
                double v = 0; std::string t;
                if (!fieldValue(f, fr, v, t)) continue;
                if (f.type == "char" || f.type == "hex") { f.lastText = t; f.seen = true; line += f.name + "=" + t + "  "; continue; }
                v = v * f.scale + f.add;
                f.last = v; f.seen = true; f.lo = std::min(f.lo, v); f.hi = std::max(f.hi, v);
                line += f.name + "=" + fmt(v) + (f.unit.empty() ? "" : " " + f.unit) + "  ";
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            o.frames.emplace_back(line.begin(), line.end());
        }
    }
};
