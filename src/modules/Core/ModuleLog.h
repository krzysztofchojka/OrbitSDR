#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <fstream>
#include <ctime>
#include "../../NativeDialogs.h"

// =====================================================================
//  Text log of a decoder (like aprs.log).
//
//  Every message is appended to a log file in the settings folder that
//  grows over all sessions, and is also kept in memory for this session,
//  so it can be exported to a file of the user's choice.
// =====================================================================

class ModuleLog {
    mutable std::mutex mtx;
    std::string path;
    std::deque<std::string> session;   // lines of this session (newest last)
    static constexpr size_t MAX_SESSION = 20000;

public:
    void setPath(const std::string& p) { std::lock_guard<std::mutex> l(mtx); path = p; }
    std::string getPath() const { std::lock_guard<std::mutex> l(mtx); return path; }

    static std::string timestamp() {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char b[32];
        std::strftime(b, sizeof(b), "[%Y-%m-%d %H:%M:%S]", &tm);
        return b;
    }

    // Appends one line (without timestamp, it is added here)
    void add(const std::string& line) {
        std::lock_guard<std::mutex> l(mtx);
        std::string full = timestamp() + " " + line;
        session.push_back(full);
        if (session.size() > MAX_SESSION) session.pop_front();
        if (path.empty()) return;
        std::ofstream f(path, std::ios::app);
        if (f.is_open()) f << full << "\n";
    }

    size_t sessionCount() const { std::lock_guard<std::mutex> l(mtx); return session.size(); }

    // Opens the whole log file (all sessions) in the default text viewer
    void openLogFile() const {
        std::string p = getPath();
        if (p.empty()) return;
        { std::ofstream touch(p, std::ios::app); }   // make sure it exists
        openWithDefaultApp(p);
    }

    // Asks for a file name and writes this session's lines into it.
    // Returns the path written, or an empty string (cancelled / nothing to save).
    std::string exportSession(const std::string& folder, const std::string& baseName) const {
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> l(mtx);
            lines.assign(session.begin(), session.end());
        }
        if (lines.empty()) {
            showPopup("Export", "Nothing received in this session yet.");
            return "";
        }
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char b[64];
        std::strftime(b, sizeof(b), "_%Y%m%d_%H%M%S.txt", &tm);
        std::string target = saveTextFileDialog(folder, baseName + b, "Export " + baseName + " messages");
        if (target.empty()) return "";
        std::ofstream f(target, std::ios::trunc);
        if (!f.is_open()) {
            showPopup("Export", "Could not write " + target);
            return "";
        }
        for (const std::string& s : lines) f << s << "\n";
        return target;
    }
};
