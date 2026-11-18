#ifndef REPORT_H
#define REPORT_H
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <vector>
#include <string>
#include <functional>

#include "tetra.h"
#include "log.h"
#include "pdu.h"

// =====================================================================
//  OrbitSDR port of tetra-kit's Report.
//
//  Upstream, Report built a rapidjson document and sent it as a JSON line
//  over a UDP socket (and used zlib to compress U-plane speech). OrbitSDR
//  hosts the decoder in-process, so this version keeps the exact add()/
//  start()/send() API every layer already calls, but collects the fields
//  into a plain structured record and hands it to an in-process sink on
//  send(). No rapidjson, no zlib, no sockets.
// =====================================================================

namespace Tetra {

    /** @brief One decoded report: its service / PDU and ordered key->value fields. */
    struct ReportData {
        std::string service;
        std::string pdu;
        std::vector<std::pair<std::string, std::string>> fields;
        std::string          blobField;   ///< name of the binary payload (U-plane: "frame")
        std::vector<uint8_t> blob;        ///< binary payload (U-plane speech frame, as built by UPlane)

        /** @brief Value for a field name, or "" if absent. */
        std::string get(const std::string& key) const {
            for (const auto& f : fields) if (f.first == key) return f.second;
            return std::string();
        }
        bool has(const std::string& key) const {
            for (const auto& f : fields) if (f.first == key) return true;
            return false;
        }
    };

    /** @brief In-process consumer of finished reports. */
    using ReportSink = std::function<void(const ReportData&)>;

    /**
     * @brief Structured in-process report (drop-in for the UDP/JSON Report)
     *
     */
    class Report {
    public:
        Report(ReportSink sink, Tetra::Log * log);
        ~Report();

        void start(const std::string service, const std::string pdu, const TetraTime tetraTime, const MacAddress macAddress);
        void startUPlane(const std::string service, const std::string pdu, const TetraTime tetraTime, const MacAddress macAddress);
        void add(std::string field, std::string val);
        void add(std::string field, uint8_t val);
        void add(std::string field, uint16_t val);
        void add(std::string field, uint32_t val);
        void add(std::string field, uint64_t val);
        void add(std::string field, double val);
        void add(std::string field, Pdu pdu);
        void addArray(std::string name, std::vector<std::tuple<std::string, uint64_t>> & infos);
        void addCompressed(std::string field, const unsigned char * binary_data, uint16_t data_len);
        void send();

    private:
        ReportSink   m_sink;                                                    ///< in-process report consumer
        Tetra::Log * m_log;                                                     ///< screen logger
        ReportData   m_cur;                                                     ///< report being built
    };
};

#endif /* REPORT_H */
