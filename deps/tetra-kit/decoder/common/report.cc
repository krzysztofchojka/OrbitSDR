#include "report.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace Tetra;

/**
 * @brief Constructor
 *
 * @param sink   In-process consumer called on send()
 * @param log    Screen logger
 *
 */

Report::Report(ReportSink sink, Tetra::Log * log)
{
    m_sink = sink;
    m_log  = log;
}

Report::~Report()
{

}

/**
 * @brief Prepare report: reset the record and add the common Tetra fields.
 *        Must be ended by send().
 *
 */

void Report::start(std::string service, std::string pdu, const TetraTime tetraTime, const MacAddress macAddress)
{
    m_cur = ReportData();
    m_cur.service = service;
    m_cur.pdu     = pdu;

    std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm* timeinfo = std::localtime(&currentTime);
    std::ostringstream oss;
    oss << std::put_time(timeinfo, "%Y-%m-%dT%H:%M:%SZ");

    add("time", oss.str());
    add("service", service);
    add("pdu",     pdu);

    add("tn", tetraTime.tn);
    add("fn", tetraTime.fn);
    add("mn", tetraTime.mn);

    add("ssi",             macAddress.ssi);
    add("usage marker",    macAddress.usageMarker);
    add("encryption mode", macAddress.encryptionMode);
    add("address_type",    macAddress.addressType);

    switch (macAddress.addressType)
    {
    case 0b001: add("actual ssi", macAddress.ssi); break;                       // SSI
    case 0b011: add("ussi", macAddress.ussi); break;                           // USSI
    case 0b100: add("smi", macAddress.smi); break;                             // SMI
    case 0b010: add("event label", macAddress.eventLabel); break;             // event label
    case 0b101:                                                                 // SSI + event label
        add("actual ssi", macAddress.ssi);
        add("event label", macAddress.eventLabel);
        break;
    case 0b110:                                                                 // SSI + usage marker
        add("actual ssi", macAddress.ssi);
        add("actual usage marker", macAddress.usageMarker);
        break;
    case 0b111:                                                                 // SMI + event label
        add("smi", macAddress.smi);
        add("event label", macAddress.eventLabel);
        break;
    }
}

/**
 * @brief Prepare U-PLANE report (no encryption-mode field; handled by MAC).
 *        Must be ended by send().
 *
 */

void Report::startUPlane(std::string service, std::string pdu, const TetraTime tetraTime, const MacAddress macAddress)
{
    m_cur = ReportData();
    m_cur.service = service;
    m_cur.pdu     = pdu;

    add("service", service);
    add("pdu",     pdu);

    add("tn", tetraTime.tn);
    add("fn", tetraTime.fn);
    add("mn", tetraTime.mn);

    add("ssi",          macAddress.ssi);
    add("usage marker", macAddress.usageMarker);
    add("address_type", macAddress.addressType);

    switch (macAddress.addressType)
    {
    case 0b001: add("actual ssi", macAddress.ssi); break;
    case 0b011: add("ussi", macAddress.ussi); break;
    case 0b100: add("smi", macAddress.smi); break;
    case 0b010: add("event label", macAddress.eventLabel); break;
    case 0b101:
        add("actual ssi", macAddress.ssi);
        add("event label", macAddress.eventLabel);
        break;
    case 0b110:
        add("actual ssi", macAddress.ssi);
        add("actual usage marker", macAddress.usageMarker);
        break;
    case 0b111:
        add("smi", macAddress.smi);
        add("event label", macAddress.eventLabel);
        break;
    }
}

void Report::add(std::string field, std::string val) { m_cur.fields.emplace_back(field, val); }
void Report::add(std::string field, uint8_t  val)    { add(field, (uint64_t)val); }
void Report::add(std::string field, uint16_t val)    { add(field, (uint64_t)val); }
void Report::add(std::string field, uint32_t val)    { add(field, (uint64_t)val); }
void Report::add(std::string field, uint64_t val)    { m_cur.fields.emplace_back(field, std::to_string(val)); }
void Report::add(std::string field, double   val)    { m_cur.fields.emplace_back(field, std::to_string(val)); }
void Report::add(std::string field, Pdu pdu)         { add(field, pdu.toHex()); }

void Report::addArray(std::string name, std::vector<std::tuple<std::string, uint64_t>> & infos)
{
    // Flatten "name[field] = val" so nothing is lost without a nested structure.
    for (std::size_t i = 0; i < infos.size(); i++)
        m_cur.fields.emplace_back(name + "[" + std::get<0>(infos[i]) + "]",
                                  std::to_string(std::get<1>(infos[i])));
}

/**
 * @brief U-plane speech payload. Upstream zlib-compressed and base64-encoded it
 *        for the JSON line; in-process it is simply handed over as raw bytes
 *        (no zlib needed). OrbitSDR decodes clear speech from it (TetraCore.h).
 *
 */

void Report::addCompressed(std::string field, const unsigned char * binaryData, uint16_t dataLen)
{
    m_cur.blobField = field;
    m_cur.blob.assign(binaryData, binaryData + dataLen);
}

void Report::send()
{
    if (m_sink) m_sink(m_cur);
}
