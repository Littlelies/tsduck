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
//  Transport stream processor shared library:
//  Extract PID's containing PSI/SI
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsCASSelectionArgs.h"
#include "tsSectionDemux.h"
#include "tsDescriptorList.h"
#include "tsPIDOperator.h"
#include "tsTables.h"
TSDUCK_SOURCE;


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

namespace ts {
    class SIFilterPlugin: public ProcessorPlugin, private TableHandlerInterface
    {
    public:
        // Implementation of plugin API
        SIFilterPlugin(TSP*);
        virtual bool start() override;
        virtual Status processPacket(TSPacket&, bool&, bool&) override;

    private:
        CASSelectionArgs _cas_args;    // CAS selection
        bool             _pass_pmt;    // Pass PIDs containing PMT
        Status           _drop_status; // Status for dropped packets
        PIDSet           _pass_pids;   // List of PIDs to pass
        SectionDemux     _demux;       // Section filter

        // Invoked by the demux when a complete table is available.
        virtual void handleTable(SectionDemux&, const BinaryTable&) override;

        // Process specific tables
        void processPAT(const PAT&);

        // Inaccessible operations
        SIFilterPlugin() = delete;
        SIFilterPlugin(const SIFilterPlugin&) = delete;
        SIFilterPlugin& operator=(const SIFilterPlugin&) = delete;
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_PROCESSOR(ts::SIFilterPlugin)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::SIFilterPlugin::SIFilterPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"Extract PID's containing the specified PSI/SI.", u"[options]"),
    _cas_args(),
    _pass_pmt(false),
    _drop_status(TSP_DROP),
    _pass_pids(),
    _demux(this)
{
    option(u"bat", 0);
    option(u"cat", 0);
    option(u"eit", 0);
    option(u"nit", 0);
    option(u"pat", 0);
    option(u"pmt", 'p');
    option(u"rst", 0);
    option(u"sdt", 0);
    option(u"stuffing", 's');
    option(u"tdt", 0);
    option(u"tot", 0);
    option(u"tsdt", 0);

    setHelp(u"Options:\n"
            u"\n"
            u"  --bat\n"
            u"      Extract PID 0x0011 (SDT/BAT).\n"
            u"\n"
            u"  --cat\n"
            u"      Extract PID 0x0001 (CAT).\n"
            u"\n"
            u"  --eit\n"
            u"      Extract PID 0x0012 (EIT).\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
            u"\n"
            u"  --nit\n"
            u"      Extract PID 0x0010 (NIT).\n"
            u"\n"
            u"  --pat\n"
            u"      Extract PID 0x0000 (PAT).\n"
            u"\n"
            u"  -p\n"
            u"  --pmt\n"
            u"      Extract all PMT PID's.\n"
            u"\n"
            u"  --rst\n"
            u"      Extract PID 0x0013 (RST).\n"
            u"\n"
            u"  --sdt\n"
            u"      Extract PID 0x0011 (SDT/BAT).\n"
            u"\n"
            u"  -s\n"
            u"  --stuffing\n"
            u"      Replace excluded packets with stuffing (null packets) instead\n"
            u"      of removing them. Useful to preserve bitrate.\n"
            u"\n"
            u"  --tdt\n"
            u"      Extract PID 0x0014 (TDT/TOT).\n"
            u"\n"
            u"  --tot\n"
            u"      Extract PID 0x0014 (TDT/TOT).\n"
            u"\n"
            u"  --tsdt\n"
            u"      Extract PID 0x0002 (TSDT).\n"
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n");

    _cas_args.defineOptions(*this);
    _cas_args.addHelp(*this);
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::SIFilterPlugin::start()
{
    // Get command line arguments
    _cas_args.load(*this);
    _pass_pmt = present(u"pmt");
    _drop_status = present(u"stuffing") ? TSP_NULL : TSP_DROP;

    _pass_pids.reset();
    if (present(u"bat")) {
        _pass_pids.set(PID_BAT);
    }
    if (present(u"cat")) {
        _pass_pids.set(PID_CAT);
    }
    if (present(u"eit")) {
        _pass_pids.set(PID_EIT);
    }
    if (present(u"nit")) {
        _pass_pids.set(PID_NIT);
    }
    if (present(u"pat")) {
        _pass_pids.set(PID_PAT);
    }
    if (present(u"rst")) {
        _pass_pids.set(PID_RST);
    }
    if (present(u"sdt")) {
        _pass_pids.set(PID_SDT);
    }
    if (present(u"tdt")) {
        _pass_pids.set(PID_TDT);
    }
    if (present(u"tot")) {
        _pass_pids.set(PID_TOT);
    }
    if (present(u"tsdt")) {
        _pass_pids.set(PID_TSDT);
    }

    // Reinitialize the demux
    _demux.reset();
    _demux.addPID(PID_PAT);
    if (_cas_args.pass_emm) {
        _demux.addPID(PID_CAT);
    }

    return true;
}


//----------------------------------------------------------------------------
// Invoked by the demux when a complete table is available.
//----------------------------------------------------------------------------

void ts::SIFilterPlugin::handleTable(SectionDemux& demux, const BinaryTable& table)
{
    switch (table.tableId()) {

        case TID_PAT: {
            PAT pat(table);
            if (pat.isValid()) {
                processPAT(pat);
            }
            break;
        }

        case TID_CAT: {
            CAT cat(table);
            if (cat.isValid()) {
                _cas_args.addMatchingPIDs(_pass_pids, cat, *tsp);
            }
            break;
        }

        case TID_PMT: {
            PMT pmt(table);
            if (pmt.isValid()) {
                _cas_args.addMatchingPIDs(_pass_pids, pmt, *tsp);
            }
            break;
        }

        default: {
            break;
        }
    }
}


//----------------------------------------------------------------------------
//  This method processes a Program Association Table (PAT).
//----------------------------------------------------------------------------

void ts::SIFilterPlugin::processPAT(const PAT& pat)
{
    for (PAT::ServiceMap::const_iterator it = pat.pmts.begin(); it != pat.pmts.end(); ++it) {
        // Add PMT PID to section filter if ECM are required
        if (_cas_args.pass_ecm) {
            _demux.addPID(it->second);
        }
        // Pass this PMT PID if PMT are required
        if (_pass_pmt && !_pass_pids[it->second]) {
            tsp->verbose(u"Filtering PMT PID %d (0x%X)", {it->second, it->second});
            _pass_pids.set(it->second);
        }
    }
}


//----------------------------------------------------------------------------
// Packet processing method
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::SIFilterPlugin::processPacket(TSPacket& pkt, bool& flush, bool& bitrate_changed)
{
    _demux.feedPacket(pkt);
    return _pass_pids[pkt.getPID()] ? TSP_OK : _drop_status;
}
