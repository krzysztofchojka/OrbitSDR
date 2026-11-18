#ifndef PDU_H
#define PDU_H
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>
#include "tetra.h"

namespace Tetra {

    /**
     * @brief Pdu base class contains functions useful for manipulating
     *        vector<uint8_t> data
     *
     */

    class Pdu {
    public:
        Pdu();
        Pdu(const std::vector<uint8_t> & val);
        Pdu(const Pdu & pdu, const uint32_t startPos, const int32_t length = 0);
        ~Pdu();

        void append(const uint8_t val);
        void append(const std::vector<uint8_t> vec);
        void append(const Pdu val);
        void clear();
        void print(const int len = 0);
        void resize(const std::size_t len);

        uint8_t at(const std::size_t pos);
        std::vector<uint8_t> extractVec(const uint32_t startPos, const int32_t length);
        uint64_t getValue(const uint64_t startPos, const uint8_t fieldLen);
        bool isEmpty();
        std::size_t size();
        std::string toHex();
        std::string toString(const int len = 0);
        void toPackedUInt8(uint8_t * data);

        // TETRA specific functions
        std::string textGsm7BitDecode(const int16_t len);
        std::string textGeneric8BitDecode(const int16_t len);
        // OrbitSDR: 8-bit text in the SDS's text coding scheme (29.5.4.1), as UTF-8.
        // 1 = ISO 8859-1 (Latin-1), 2 = ISO 8859-2 (Latin-2: Polish, Czech ...);
        // other schemes fall back to textGeneric8BitDecode.
        std::string textIso8859Decode(const int16_t len, const uint8_t codingScheme);
        std::string locationNmeaDecode(const int16_t len);

    private:
        std::vector<uint8_t> m_vec;                                             ///< Internal PDU data storage as vector of uint8_t
    };
};

#endif /* PDU_H */
