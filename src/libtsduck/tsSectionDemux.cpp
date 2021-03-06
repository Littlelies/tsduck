//----------------------------------------------------------------------------
//
// TSDuck - The MPEG Transport Stream Toolkit
// Copyright (c) 2005-2018, Thierry Lelegard
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//
//----------------------------------------------------------------------------
//
//  This class rebuilds MPEG tables and sections from TS packets
//
//----------------------------------------------------------------------------

#include "tsSectionDemux.h"
TSDUCK_SOURCE;


//----------------------------------------------------------------------------
// Demux status information - Default constructor
//----------------------------------------------------------------------------

ts::SectionDemux::Status::Status() :
    invalid_ts(0),
    discontinuities(0),
    scrambled(0),
    inv_sect_length(0),
    inv_sect_index(0),
    wrong_crc(0)
{
}


//----------------------------------------------------------------------------
// Demux status information
// Constructor from the current status of SectionDemux
//----------------------------------------------------------------------------

ts::SectionDemux::Status::Status(const SectionDemux& demux) :
    invalid_ts(0),
    discontinuities(0),
    scrambled(0),
    inv_sect_length(0),
    inv_sect_index(0),
    wrong_crc(0)
{
    demux.getStatus(*this);
}


//----------------------------------------------------------------------------
// Demux status information - Reset content
//----------------------------------------------------------------------------

void ts::SectionDemux::Status::reset()
{
    invalid_ts = 0;
    discontinuities = 0;
    scrambled = 0;
    inv_sect_length = 0;
    inv_sect_index = 0;
    wrong_crc = 0;
}


//----------------------------------------------------------------------------
// Demux status information - Check if any counter is non zero
//----------------------------------------------------------------------------

bool ts::SectionDemux::Status::hasErrors() const
{
    return
        invalid_ts != 0 ||
        discontinuities != 0 ||
        scrambled != 0 ||
        inv_sect_length != 0 ||
        inv_sect_index != 0 ||
        wrong_crc != 0;
}


//----------------------------------------------------------------------------
// Display content of a status block.
// Indent indicates the base indentation of lines.
// If errors_only is true, don't report zero counters.
//----------------------------------------------------------------------------

std::ostream& ts::SectionDemux::Status::display(std::ostream& strm, int indent, bool errors_only) const
{
    const std::string margin(indent, ' ');

    if (!errors_only || invalid_ts != 0) {
        strm << margin << "Invalid TS packets: " << UString::Decimal(invalid_ts) << std::endl;
    }
    if (!errors_only || discontinuities != 0) {
        strm << margin << "TS packets discontinuities: " << UString::Decimal(discontinuities) << std::endl;
    }
    if (!errors_only || scrambled != 0) {
        strm << margin << "Scrambled TS packets: " << UString::Decimal(scrambled) << std::endl;
    }
    if (!errors_only || inv_sect_length != 0) {
        strm << margin << "Invalid section lengths: " << UString::Decimal(inv_sect_length) << std::endl;
    }
    if (!errors_only || inv_sect_index != 0) {
        strm << margin << "Invalid section index: " << UString::Decimal(inv_sect_index) << std::endl;
    }
    if (!errors_only || wrong_crc != 0) {
        strm << margin << "Corrupted sections (bad CRC): " << UString::Decimal(wrong_crc) << std::endl;
    }

    return strm;
}


//----------------------------------------------------------------------------
// SectionDemux constructor.
//----------------------------------------------------------------------------

ts::SectionDemux::SectionDemux(TableHandlerInterface* table_handler,
                               SectionHandlerInterface* section_handler,
                               const PIDSet& pid_filter) :
    SuperClass(pid_filter),
    _table_handler(table_handler),
    _section_handler(section_handler),
    _pids (),
    _status (),
    _packet_count (0)
{
}


//----------------------------------------------------------------------------
// Destructor.
//----------------------------------------------------------------------------

ts::SectionDemux::~SectionDemux ()
{
}


//----------------------------------------------------------------------------
// Reset the analysis context (partially built sections and tables).
//----------------------------------------------------------------------------

void ts::SectionDemux::immediateReset()
{
    _pids.clear();
}

void ts::SectionDemux::immediateResetPID(PID pid)
{
    _pids.erase(pid);
}


//----------------------------------------------------------------------------
// Feed the depacketizer with a TS packet.
//----------------------------------------------------------------------------

void ts::SectionDemux::feedPacket(const TSPacket& pkt)
{
    if (_pid_filter[pkt.getPID()]) {
        processPacket(pkt);
    }
    _packet_count++;
}

void ts::SectionDemux::processPacket(const TSPacket& pkt)
{
    // Reject invalid packets

    if (!pkt.hasValidSync()) {
        _status.invalid_ts++;
        return;
    }

    // Get PID and reference to the PID context.
    // The PID context is created if did not exist.

    PID pid = pkt.getPID();
    PIDContext& pc(_pids[pid]);

    // If TS packet is scrambled, we cannot decode it and we loose
    // synchronization on this PID (usually, PID's carrying sections
    // are not scrambled).

    if (pkt.getScrambling ()) {
        _status.scrambled++;
        pc.syncLost ();
        return;
    }

    // Check continuity counter on this PID (only if we have not lost
    // the synchronization on this PID).

    if (pc.sync) {
        // Ignore duplicate packets (same CC)
        if (pkt.getCC() == pc.continuity) {
            return;
        }
        // Check if we are still synchronized
        if (pkt.getCC () != (pc.continuity + 1) % CC_MAX) {
            _status.discontinuities++;
            pc.syncLost ();
        }
    }

    pc.continuity = pkt.getCC ();

    // Locate TS packet payload

    size_t header_size = pkt.getHeaderSize ();

    if (!pkt.hasPayload () || header_size >= PKT_SIZE) {
        return;
    }

    uint8_t pointer_field;
    const uint8_t* payload;
    size_t payload_size;
    PacketCounter pusi_pkt_index = pc.pusi_pkt_index;

    if (pkt.getPUSI ()) {
        // Keep track of last packet containing a PUSI in this PID
        pc.pusi_pkt_index = _packet_count;
        // Payload Unit Start Indicator (PUSI) is set.
        // Filter out PES packets. A PES packet starts with the "start code prefix"
        // 00 00 01. This sequence cannot be found in a TS packet with sections
        // (would be 00 = pointer field, 00 = PAT, 01 = not possible for a PAT).
        if (header_size + 3 <= PKT_SIZE &&
            pkt.b[header_size] == 0x00 &&
            pkt.b[header_size + 1] == 0x00 &&
            pkt.b[header_size + 2] == 0x01) {
            // Losing sync, will skip all TS packet until next PUSI
            pc.syncLost ();
            return;
        }
        // First byte of payload is a pointer field
        pointer_field = pkt.b[header_size];
        payload = pkt.b + header_size + 1;
        payload_size = PKT_SIZE - header_size - 1;
        // Ignore packet and loose sync if inconsistent pointer field
        if (pointer_field >= payload_size) {
            pc.syncLost ();
            return;
        }
        if (pointer_field == 0) {
            pusi_pkt_index = _packet_count;
        }
    }
    else {
        // PUSI not set, first byte of payload is section data
        pointer_field = 0xFF;
        payload = pkt.b + header_size;
        payload_size = PKT_SIZE - header_size;
    }

    if (payload_size <= 0) {
        return;
    }

    // If no previous synchronization, skip incomplete sections

    if (!pc.sync) {
        // If no new section in this packet, ignore it
        if (!pkt.getPUSI ()) {
            return;
        }
        // Skip end of previous section
        payload += pointer_field;
        payload_size -= pointer_field;
        pointer_field = 0;
        // We have found the beginning of a section, we are now synchronized
        pc.sync = true;
    }

    // Copy TS packet payload in PID context

    pc.ts.append (payload, payload_size);

    // Locate TS buffer by address and size.

    const uint8_t* ts_start = pc.ts.data();
    size_t ts_size = pc.ts.size();

    // If current packet has a PUSI, locate start of this new section
    // inside the TS buffer. This is not useful to locate the section but
    // it is used to check that the previous section was not truncated
    // (example: detect incorrect stream as generated with old version
    // of Thomson Grass Valley NetProcessor).

    const uint8_t* pusi_section = 0;

    if (pkt.getPUSI()) {
        pusi_section = ts_start + ts_size - payload_size + pointer_field;
    }

    // Loop on all complete sections in the buffer.
    // If there is less than 3 bytes in the buffer, we cannot even
    // determine the section length.

    while (ts_size >= 3) {

        // Get section header.

        bool section_ok = true;
        ETID etid(ts_start[0]);
        uint16_t section_length = GetUInt16(ts_start + 1);
        bool long_header = (section_length & 0x8000) != 0;
        section_length = (section_length & 0x0FFF) + SHORT_SECTION_HEADER_SIZE;

        // Lose synchronization when invalid section length.

        if (section_length > MAX_PRIVATE_SECTION_SIZE ||
            section_length < MIN_SHORT_SECTION_SIZE ||
            (long_header && section_length < MIN_LONG_SECTION_SIZE)) {

            _status.inv_sect_length++;
            pc.syncLost ();
            return;
        }

        // Exit when end of section is missing. Wait for next TS packets.

        if (ts_size < section_length) {
            break;
        }

        // If we detect that the section is incorrectly truncated, skip it.

        if (pusi_section != 0 && ts_start < pusi_section && ts_start + section_length > pusi_section) {
            section_ok = false;
            // Resynchronize to actual section start
            section_length = uint16_t(pusi_section - ts_start);
        }

        // We have a complete section in the pc.ts buffer. Analyze it.

        uint8_t version = 0;
        bool is_next = false;
        uint8_t section_number = 0;
        uint8_t last_section_number = 0;

        if (section_ok && long_header) {
            etid = ETID (etid.tid(), GetUInt16 (ts_start + 3));
            version = (ts_start[5] >> 1) & 0x1F;
            is_next = (ts_start[5] & 0x01) == 0;
            section_number = ts_start[6];
            last_section_number = ts_start[7];
            // Check that the section number fits in the range
            if (section_number > last_section_number) {
                _status.inv_sect_index++;
                section_ok = false;
            }
        }

        // Sections with the 'next' indicator are ignored

        if (is_next) {
            section_ok = false;
        }

        if (section_ok) {

            // Get reference to the ETID context for this PID.
            // The ETID context is created if did not exist.

            ETIDContext& tc (pc.tids[etid]);

            // If this is a new version of the table, reset the TID context.
            // Note that short sections do not have versions, so the version
            // field is implicitely zero. However, every short section must
            // be considered as a new version since there is no way to track
            // versions.

            if (!long_header ||             // short section
                tc.sect_expected == 0 ||    // new TID on this PID
                tc.version != version) {    // new version

                tc.version = version;
                tc.sect_expected = size_t (last_section_number) + 1;
                tc.sect_received = 0;
                tc.sects.resize (tc.sect_expected);
                // Mark all section entries as unused
                for (size_t si = 0; si < tc.sect_expected; si++) {
                    tc.sects[si].reset();
                }
            }

            // Check that the total number of sections in the table
            // has not changed since last section.

            if (last_section_number != tc.sect_expected - 1) {
                _status.inv_sect_index++;
                section_ok = false;
            }

            // Create a new Section object if necessary (ie. if a section
            // hendler is registered or if this is a new section).

            SectionPtr sect_ptr;

            if (section_ok && (_section_handler != 0 || tc.sects[section_number].isNull())) {
                sect_ptr = new Section (ts_start, section_length, pid, CRC32::CHECK);
                sect_ptr->setFirstTSPacketIndex (pusi_pkt_index);
                sect_ptr->setLastTSPacketIndex (_packet_count);
                if (!sect_ptr->isValid()) {
                    _status.wrong_crc++;  // only possible error (hum?)
                    section_ok = false;
                }
            }

            // Mark that we are in the context of a table or section handler.
            // This is used to prevent the destruction of PID contexts during
            // the execution of a handler.
            beforeCallingHandler(pid);
            try {
                // If a handler is defined for sections, invoke it.
                if (section_ok && _section_handler != 0) {
                    _section_handler->handleSection (*this, *sect_ptr);
                }

                // Save the section in the TID context if this is a new one.
                if (section_ok && tc.sects[section_number].isNull()) {

                    // Save the section
                    tc.sects[section_number] = sect_ptr;
                    tc.sect_received++;

                    // If the table is completed and a handler is present, build the table.
                    if (tc.sect_received == tc.sect_expected && _table_handler != 0) {

                        // Build the table
                        BinaryTable table;
                        for (size_t i = 0; i < tc.sects.size(); ++i) {
                            table.addSection (tc.sects[i]);
                        }

                        // Invoke the table handler
                        _table_handler->handleTable (*this, table);
                    }
                }
            }
            catch (...) {
                afterCallingHandler(false);
                throw;
            }
            if (afterCallingHandler(true)) {
                return;  // the PID of this packet or the complete demux was reset.
            }
        }

        // Move to next section in the buffer

        ts_start += section_length;
        ts_size -= section_length;

        // The next section necessarily starts in current packet
        pusi_pkt_index = _packet_count;

        // If start of next area is 0xFF (invalid TID value), the rest of
        // the packet is stuffing. Skip it.

        if (ts_size > 0 && ts_start[0] == 0xFF) {
            ts_size = 0;
        }
    }

    // If an incomplete section remains in the buffer, move it back to
    // the start of the buffer.

    if (ts_size <= 0) {
        // TS buffer becomes empty
        pc.ts.clear();
    }
    else if (ts_start > pc.ts.data()) {
        // Remove start of TS buffer
        pc.ts.erase(0, ts_start - pc.ts.data());
    }
}
