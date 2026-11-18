/*
 *  tetra-kit
 *  Copyright (C) 2020  LarryTh <dev@logami.fr>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef MAC_DEFRAG_H
#define MAC_DEFRAG_H
#include <cstdint>
#include <vector>
#include <string>
#include "../common/tetra.h"
#include "../common/pdu.h"
#include "../common/log.h"                                                      // TK_PRINTF (OrbitSDR port)

namespace Tetra {
    
    /**
     * @brief MAC defragmenter
     *
     */

    class MacDefrag {
    public:
        MacDefrag(int debug_level);
        ~MacDefrag();

        MacAddress macAddress;                                                  // MAC address
        TetraTime  startTime;                                                   // start time of defragemnter (will be used to stop on missing/invalid end frag packet receive)

        void start(const MacAddress address, const TetraTime timeSlot);
        void append(Pdu sdu, const MacAddress address);
        void stop();

        Pdu getSdu(uint8_t * encryptionMode, uint8_t * usageMarker);

    private:
        Pdu m_sdu;                                                              // reconstructed TM-SDU to be transfered to LLC

        int g_debug_level;
        bool b_stopped;
        uint8_t m_fragmentsCount;
    };

};

#endif /* MAC_DEFRAG_H */
