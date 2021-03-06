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
//  DVB-CSA (Common Scrambling Algorithm) Scrambler
//
//----------------------------------------------------------------------------

#include "tsPlugin.h"
#include "tsScrambling.h"
#include "tsByteBlock.h"
#include "tsService.h"
#include "tsSectionDemux.h"
#include "tsCyclingPacketizer.h"
#include "tsOneShotPacketizer.h"
#include "tsECMGClient.h"
#include "tsSystemRandomGenerator.h"
#include "tsCADescriptor.h"
#include "tsPAT.h"
#include "tsPMT.h"
#include "tsSDT.h"
TSDUCK_SOURCE;

#define DEFAULT_ECM_BITRATE 30000
#define ASYNC_HANDLER_EXTRA_STACK_SIZE (1024 * 1024)


//----------------------------------------------------------------------------
// Plugin definition
//----------------------------------------------------------------------------

// Notes on crypto-period dynamics:
//
// A crypto-period is defined using a CryptoPeriod object (private class inside
// ScramblerPlugin). It contains: crypto-period number, current/next CW and ECM
// containing these two CW.
//
// It is necessary to maintain two CryptoPeriod objects.
// During crypto-period N, designated as cp(N):
// - Scrambling is performed using CW(N).
// - At beginning of cp(N), if delay_start > 0, we broadcast ECM(N-1).
// - In middle of cp(N), we broadcast ECM(N).
// - At end of cp(N), if delay_start < 0, we broadcast ECM(N+1).
//
// So, during cp(N), we need cp(N-1)/cp(N), then cp(N)/cp(N+1). On a dynamic
// standpoint, as soon as ECM(N-1) is no longer needed, we generate cp(N+1).
// In asynchronous mode, there is enough time to generate ECM(N+1) while
// cp(N) is finishing.
//
// The transition points in the TS are:
// - CW change (start a new crypto-period)
// - ECM change (start broadcasting a new ECM, can be before or after
//   start of crypto-period, depending on delay_start).
//
// Entering "degraded mode":
// In asynchronous mode (the default), an ECM is actually returned by the ECMG
// long after it has been submitted. To complete a transition CW(N) -> CW(N+1)
// or ECM(N) -> ECM(N+1), we check that ECM(N+1) is ready. If it is not, we
// enter "degraded mode". In this mode, no transition is allowed, the same CW
// and ECM are used until exit of the degraded mode. This can occur when an
// ECM takes too long to be ciphered.
//
// Exiting "degraded mode":
// When in degraded mode, each time an ECM(N) packet is inserted, we check if
// ECM(N+1) is ready. When it is ready, we exit degraded mode. If delay_start
// is negative, we immediately perform an ECM transition and we recompute the
// time for the next CW transition. If delay_start is positive, we immediately
// perform a CW transition and we recompute the time for the next ECM transition.

namespace ts {
    class ScramblerPlugin: public ProcessorPlugin, private TableHandlerInterface
    {
    public:
        // Implementation of plugin API
        ScramblerPlugin(TSP*);
        virtual bool start() override;
        virtual bool stop() override;
        virtual Status processPacket(TSPacket&, bool&, bool&) override;

    private:
        // Description of a crypto-period.
        // Each CryptoPeriod object points to its ScramblerPlugin parent object.
        // In case of error in a CryptoPeriod object, the _abort volatile flag
        // is set in ScramblerPlugin.
        class CryptoPeriod: private ECMGClientHandlerInterface
        {
        public:
            // Default constructor.
            CryptoPeriod();

            // Initialize first crypto period.
            // Generate two randow CW and corresponding ECM.
            // ECM generation may complete asynchronously.
            void initCycle(ScramblerPlugin*, uint16_t cp_number);

            // Initialize crypto period following specified one.
            // ECM generation may complete asynchronously.
            void initNext(const CryptoPeriod&);

            // Check if ECM generation is complete (useful in asynchronous mode)
            bool ecmReady() const { return _ecm_ok; }

            // Get next ECM packet in ECM cycle (or null packet if ECM not ready).
            void getNextECMPacket(TSPacket&);

            // Initialize the scrambler with the current control word.
            void initScramblerKey() const;

            // Get scrambling control value for scrambled TS packets
            uint8_t getScramblingControlValue() const { return uint8_t((_cp_number & 0x01) ? SC_ODD_KEY : SC_EVEN_KEY); }

        private:
            ScramblerPlugin* _scrambler;      // Reference to scrambler plugin
            uint16_t         _cp_number;      // Crypto-period number
            volatile bool    _ecm_ok;         // _ecm field is valid
            TSPacketVector   _ecm;            // Packetized ECM
            size_t           _ecm_pkt_index;  // Next ECM packet to insert in TS
            uint8_t          _cw_current[CW_BYTES];
            uint8_t          _cw_next[CW_BYTES];

            // Generate the ECM for a crypto-period.
            // With --synchronous, the ECM is directly generated. Otherwise,
            // the ECM will be set later, notified through private handleECM.
            void generateECM();

            // Invoked when an ECM is available, maybe in the context of an external thread.
            virtual void handleECM(const ecmgscs::ECMResponse&) override;

            // Inaccessible operations
            CryptoPeriod(const CryptoPeriod&) = delete;
            CryptoPeriod& operator=(const CryptoPeriod&) = delete;
        };

        // ScramblerPlugin parameters, remain constant after start()
        Service           _service;            // Service description
        bool              _component_level;    // Insert CA_descriptors at component level
        bool              _use_fixed_key;      // Use a fixed control word
        bool              _scramble_audio;     // Scramble all audio components
        bool              _scramble_video;     // Scramble all video components
        bool              _scramble_subtitles; // Scramble all subtitles components
        bool              _synchronous_ecmg;   // Synchronous ECM generation
        bool              _ignore_scrambled;   // Ignore packets which are already scrambled
        SocketAddress     _ecmg_addr;          // ECMG socket address
        uint32_t          _super_cas_id;       // CA system & subsystem id
        ByteBlock         _access_criteria;    // AC constant value
        ByteBlock         _ca_desc_private;    // Private data to insert in CA_descriptor
        MilliSecond       _cp_duration;        // Crypto-period duration
        MilliSecond       _delay_start;        // Delay between CP start and ECM start (can be negative)
        BitRate           _ecm_bitrate;        // ECM PID's bitrate
        PID               _ecm_pid;            // PID for ECM
        PacketCounter     _partial_scrambling; // Do not scramble all packets if > 1
        Scrambling::EntropyMode _cw_mode;      // Entropy reduction
        ecmgscs::ChannelStatus  _channel_status; // Initial response to ECMG channel_setup
        ecmgscs::StreamStatus   _stream_status;  // Initial response to ECMG stream_setup

        // ScramblerPlugin state
        volatile bool     _abort;              // Error (service not found, etc)
        bool              _ready;              // Ready to transmit packets
        bool              _degraded_mode;      // In degraded mode (see comments above)
        PacketCounter     _packet_count;       // Complete TS packet counter
        PacketCounter     _scrambled_count;    // Summary of scrambled packets
        PacketCounter     _partial_clear;      // How many clear packets to keep clear
        PacketCounter     _pkt_insert_ecm;     // Insertion point for next ECM packet.
        PacketCounter     _pkt_change_cw;      // Transition point for next CW change
        PacketCounter     _pkt_change_ecm;     // Transition point for next ECM change
        BitRate           _ts_bitrate;         // Saved TS bitrate
        ECMGClient        _ecmg;               // Connection with the ECMG
        uint8_t           _ecm_cc;             // Continuity counter in ECM PID.
        PIDSet            _scrambled_pids;     // List of pids to scramble
        PIDSet            _conflict_pids;      // List of pids to scramble with scrambled input packets
        PIDSet            _input_pids;         // List of input pids
        CryptoPeriod      _cp[2];              // Previous/current or current/next crypto-periods
        size_t            _current_cw;         // Index to current CW (current crypto period)
        size_t            _current_ecm;        // Index to current ECM (ECM being broadcast)
        Scrambling        _current_key;        // Preprocessed current control word
        SectionDemux      _demux;              // Section demux
        CyclingPacketizer _pzer_pmt;           // Packetizer for modified PMT
        SystemRandomGenerator _cw_gen;         // Control word generator

        // Return current/next CryptoPeriod for CW or ECM
        CryptoPeriod& currentCW()  {return _cp[_current_cw];}
        CryptoPeriod& nextCW()     {return _cp[(_current_cw + 1) & 0x01];}
        CryptoPeriod& currentECM() {return _cp[_current_ecm];}
        CryptoPeriod& nextECM()    {return _cp[(_current_ecm + 1) & 0x01];}

        // Perform CW and ECM transition
        void changeCW();
        void changeECM();

        // Check if we are in degraded mode or if we enter degraded mode
        bool inDegradedMode();

        // Try to exit from degraded mode
        void tryExitDegradedMode();

        // Invoked by the demux when a complete table is available.
        virtual void handleTable(SectionDemux&, const BinaryTable&) override;

        // Process specific tables
        void processPAT(PAT&);
        void processPMT(PMT&);
        void processSDT(SDT&);

        // Inaccessible operations
        ScramblerPlugin() = delete;
        ScramblerPlugin(const ScramblerPlugin&) = delete;
        ScramblerPlugin& operator=(const ScramblerPlugin&) = delete;
    };
}

TSPLUGIN_DECLARE_VERSION
TSPLUGIN_DECLARE_PROCESSOR(ts::ScramblerPlugin)


//----------------------------------------------------------------------------
// Constructor
//----------------------------------------------------------------------------

ts::ScramblerPlugin::ScramblerPlugin(TSP* tsp_) :
    ProcessorPlugin(tsp_, u"DVB scrambler.", u"[options] service"),
    _service(),
    _component_level(false),
    _use_fixed_key(false),
    _scramble_audio(false),
    _scramble_video(false),
    _scramble_subtitles(false),
    _synchronous_ecmg(false),
    _ignore_scrambled(false),
    _ecmg_addr(),
    _super_cas_id(0),
    _access_criteria(),
    _ca_desc_private(),
    _cp_duration(0),
    _delay_start(0),
    _ecm_bitrate(0),
    _ecm_pid(PID_NULL),
    _partial_scrambling(0),
    _cw_mode(Scrambling::REDUCE_ENTROPY),
    _channel_status(),
    _stream_status(),
    _abort(false),
    _ready(false),
    _degraded_mode(false),
    _packet_count(0),
    _scrambled_count(0),
    _partial_clear(0),
    _pkt_insert_ecm(0),
    _pkt_change_cw(0),
    _pkt_change_ecm(0),
    _ts_bitrate(0),
    _ecmg(ASYNC_HANDLER_EXTRA_STACK_SIZE),
    _ecm_cc(0),
    _scrambled_pids(),
    _conflict_pids(),
    _input_pids(),
    _cp(),
    _current_cw(0),
    _current_ecm(0),
    _current_key(),
    _demux(this),
    _pzer_pmt(),
    _cw_gen()
{
    option(u"",                      0,  STRING, 1, 1);
    option(u"access-criteria",      'a', STRING);
    option(u"bitrate-ecm",          'b', POSITIVE);
    option(u"channel-id",            0,  UINT16);
    option(u"component-level",       0);
    option(u"control-word",         'c', STRING);
    option(u"cp-duration",          'd', POSITIVE);
    option(u"ecm-id",               'i', UINT16);
    option(u"ecmg",                 'e', STRING);
    option(u"ecmg-scs-version",     'v', INTEGER, 0, 1, 2, 3);
    option(u"ignore-scrambled",      0);
    option(u"no-audio",              0);
    option(u"no-entropy-reduction", 'n');
    option(u"no-video",              0);
    option(u"partial-scrambling",    0,  POSITIVE);
    option(u"pid-ecm",               0,  PIDVAL);
    option(u"private-data",         'p', STRING);
    option(u"stream-id",             0,  UINT16);
    option(u"subtitles",             0);
    option(u"super-cas-id",         's', UINT32);
    option(u"synchronous",           0);

    setHelp(u"Service:\n"
            u"  Specifies the service to scramble.\n"
            u"  If the argument is an integer value (either decimal or hexadecimal), it is\n"
            u"  interpreted as a service id. Otherwise, it is interpreted as a service name,\n"
            u"  as specified in the SDT. The name is not case sensitive and blanks are\n"
            u"  ignored. If the input TS does not contain an SDT, use service ids only.\n"
            u"\n"
            u"Options:\n"
            u"\n"
            u"  -a value\n"
            u"  --access-criteria value\n"
            u"      Specifies the access criteria for the service as sent to the ECMG.\n"
            u"      The value must be a suite of hexadecimal digits.\n"
            u"\n"
            u"  -b value\n"
            u"  --bitrate-ecm value\n"
            u"      Specifies the bitrate for ECM PID's in bits / second. The default is\n"
            u"      " + UString::Decimal(DEFAULT_ECM_BITRATE) + u" b/s.\n"
            u"\n"
            u"  --channel-id value\n"
            u"      Specifies the DVB SimulCrypt ECM_channel_id for the ECMG (default: 1).\n"
            u"\n"
            u"  -d seconds\n"
            u"  --cp-duration seconds\n"
            u"      Specifies the crypto-period duration in seconds (default: 10).\n"
            u"\n"
            u"  --component-level\n"
            u"      Add CA_descriptors at component level in the PMT. By default, the\n"
            u"      CA_descriptor is added at program level.\n"
            u"\n"
            u"  -c value\n"
            u"  --control-word value\n"
            u"      Specifies a fixed and constant control word for all TS packets.\n"
            u"      The value must be a string of 16 hexadecimal digits. When using\n"
            u"      this option, no ECMG is required.\n"
            u"\n"
            u"  -i value\n"
            u"  --ecm-id value\n"
            u"      Specifies the DVB SimulCrypt ECM_id for the ECMG (default: 1).\n"
            u"\n"
            u"  -e host:port\n"
            u"  --ecmg host:port\n"
            u"      Specify an ECM Generator. Without ECMG, a fixed control word must be\n"
            u"      specified using --control-word.\n"
            u"\n"
            u"  -v value\n"
            u"  --ecmg-scs-version value\n"
            u"      Specifies the version of the ECMG <=> SCS DVB SimulCrypt protocol.\n"
            u"      Valid values are 2 and 3. The default is 2.\n"
            u"\n"
            u"  --help\n"
            u"      Display this help text.\n"
            u"\n"
            u"  --ignore-scrambled\n"
            u"      Ignore packets which are already scrambled. Since these packets\n"
            u"      are likely scrambled with a different control word, descrambling\n"
            u"      will not be possible the usual way.\n"
            u"\n"
            u"  --no-audio\n"
            u"      Do not scramble audio components in the selected service. By default,\n"
            u"      all audio components are scrambled.\n"
            u"\n"
            u"  -n\n"
            u"  --no-entropy-reduction\n"
            u"      Do not perform CW entropy reduction to 48 bits. Keep full 64-bits CW.\n"
            u"\n"
            u"  --no-video\n"
            u"      Do not scramble video components in the selected service. By default,\n"
            u"      all video components are scrambled.\n"
            u"\n"
            u"  --partial-scrambling count\n"
            u"      Do not scramble all packets, only one packet every \"count\" packets.\n"
            u"      The default value is 1, meaning that all packets are scrambled.\n"
            u"      Specifying higher values is a way to reduce the scrambling CPU load\n"
            u"      while keeping the service mostly scrambled.\n"
            u"\n"
            u"  --pid-ecm value\n"
            u"      Specifies the new ECM PID for the service. By defaut, use the first\n"
            u"      unused PID immediately following the PMT PID. Using the default, there\n"
            u"      is a risk to later discover that this PID is already used. In that case,\n"
            u"      specify --pid-ecm with a notoriously unused PID value.\n"
            u"\n"
            u"  -p value\n"
            u"  --private-data value\n"
            u"      Specifies the private data to insert in the CA_descriptor in the PMT.\n"
            u"      The value must be a suite of hexadecimal digits.\n"
            u"\n"
            u"  --stream-id value\n"
            u"      Specifies the DVB SimulCrypt ECM_stream_id for the ECMG (default: 1).\n"
            u"\n"
            u"  --subtitles\n"
            u"      Scramble subtitles components in the selected service. By default, the\n"
            u"      subtitles components are not scrambled.\n"
            u"\n"
            u"  -s value\n"
            u"  --super-cas-id value\n"
            u"      Specify the DVB SimulCrypt Super_CAS_Id. This is required when --ecmg\n"
            u"      is specified.\n"
            u"\n"
            u"  --synchronous\n"
            u"      Specify to synchronously generate the ECM's. By default, continue\n"
            u"      processing packets while generating ECM's. Use this option with\n"
            u"      offline packet processing. Use the default (asynchronous) with live\n"
            u"      packet processing.\n"
            u"\n"
            u"  --version\n"
            u"      Display the version number.\n");
}


//----------------------------------------------------------------------------
// Start method
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::start()
{
    // Reset states
    _scrambled_pids.reset();
    _conflict_pids.reset();
    _packet_count = 0;
    _scrambled_count = 0;
    _ecm_cc = 0;
    _abort = false;
    _ready = false;
    _degraded_mode = false;
    _ts_bitrate = 0;
    _pkt_insert_ecm = 0;
    _pkt_change_cw = 0;
    _pkt_change_ecm = 0;
    _partial_clear = 0;

    // Parameters
    _service.set (value(u""));
    _use_fixed_key = present(u"control-word");
    _synchronous_ecmg = present(u"synchronous");
    _cw_mode = present(u"no-entropy-reduction") ? Scrambling::FULL_CW : Scrambling::REDUCE_ENTROPY;
    _component_level = present(u"component-level");
    _scramble_audio = !present(u"no-audio");
    _scramble_video = !present(u"no-video");
    _scramble_subtitles = present(u"subtitles");
    _partial_scrambling = intValue<PacketCounter>(u"partial-scrambling", 1);
    _ignore_scrambled = present(u"ignore-scrambled");
    _ecm_pid = intValue<PID>(u"pid-ecm", PID_NULL);
    _ecm_bitrate = intValue<BitRate>(u"bitrate-ecm", DEFAULT_ECM_BITRATE);
    _cp_duration = 1000 * intValue<MilliSecond>(u"cp-duration", 10);
    _delay_start = 0;
    _super_cas_id = intValue<uint32_t>(u"super-cas-id");
    const uint16_t ecm_channel_id = intValue<uint16_t>(u"channel-id", 1);
    const uint16_t ecm_stream_id = intValue<uint16_t>(u"stream-id", 1);
    const uint16_t ecm_id = intValue<uint16_t>(u"ecm-id", 1);

    if (!value(u"access-criteria").hexaDecode(_access_criteria)) {
        tsp->error(u"invalid access criteria, specify an even number of hexa digits");
        return false;
    }

    if (!value(u"private-data").hexaDecode(_ca_desc_private)) {
        tsp->error(u"invalid private data for CA_descriptor, specify an even number of hexa digits");
        return false;
    }

    // Specify which ECMG <=> SCS version to use.
    ecmgscs::Protocol::Instance()->setVersion(intValue<tlv::VERSION>(u"ecmg-scs-version", 2));

    // Get control word generation mechanism
    if (_use_fixed_key) {

        // Use a fixed control word
        ByteBlock cw;
        if (!value(u"control-word").hexaDecode(cw) || cw.size() != CW_BYTES) {
            tsp->error(u"invalid control word, specify 16 hexa digits");
            return false;
        }

        // Initialize current scrambling key
        _current_key.init(cw.data(), _cw_mode);
        tsp->verbose(u"using fixed control word: " + UString::Dump(cw, UString::SINGLE_LINE));
    }
    else if (!present(u"ecmg")) {
        // No --cw, no --ecmg, how do we do?
        tsp->error(u"specify either --control-word or --ecmg");
        return false;
    }
    else if (!_ecmg_addr.resolve(value(u"ecmg"), *tsp)) {
        // Invalid host:port, error message already reported
        return false;
    }
    else if (!present(u"super-cas-id")) {
        tsp->error(u"--super-cas-id is required with --ecmg");
        return false;
    }
    else if (!_ecmg.connect(_ecmg_addr, _super_cas_id, ecm_channel_id, ecm_stream_id, ecm_id,
                            uint16_t(_cp_duration / 100), _channel_status, _stream_status, tsp, tsp))
    {
        // Error connecting to ECMG, error message already reported
        return false;
    }
    else {
        // Now correctly connected to ECMG.
        // Validate delay start.
        _delay_start = MilliSecond(_channel_status.delay_start);
        if (_delay_start > _cp_duration / 2) {
            _delay_start = _cp_duration / 2;
        }
        else if (_delay_start < -_cp_duration / 2) {
            _delay_start = -_cp_duration / 2;
        }
        tsp->debug(u"crypto-period duration: %'d ms, delay start: %'d ms", {_cp_duration, _delay_start});

        // The PMT will be modified, initialize the PMT packetizer
        _pzer_pmt.reset();
        _pzer_pmt.setStuffingPolicy(CyclingPacketizer::ALWAYS);

        // Create first and second crypto-periods
        _current_cw = 0;
        _current_ecm = 0;
        _cp[0].initCycle(this, 0);
        _cp[0].initScramblerKey();
        _cp[1].initNext(_cp[0]);
    }

    // Initialize the demux.
    // If the service is known by name, filter the SDT, otherwise filter the PAT.
    _demux.reset();
    _demux.addPID(PID(_service.hasName() ? PID_SDT : PID_PAT));

    // Initialize the list of used pids. Preset reserved PIDs.
    _input_pids.reset();
    _input_pids.set(PID_NULL);
    for (PID pid = 0; pid < 0x001F; ++pid) {
        _input_pids.set (pid);
    }

    return !_abort;
}


//----------------------------------------------------------------------------
// Stop method
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::stop()
{
    // Disconnect from ECMG
    if (_ecmg.isConnected()) {
        _ecmg.disconnect();
    }

    tsp->debug(u"scrambled %'d packets in %'d PID's", {_scrambled_count, _scrambled_pids.count()});
    return true;
}


//----------------------------------------------------------------------------
// Invoked by the demux when a complete table is available.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::handleTable(SectionDemux& demux, const BinaryTable& table)
{
    switch (table.tableId()) {

        case TID_PAT: {
            if (table.sourcePID() == PID_PAT) {
                PAT pat(table);
                if (pat.isValid()) {
                    processPAT(pat);
                }
            }
            break;
        }

        case TID_SDT_ACT: {
            if (table.sourcePID() == PID_SDT) {
                SDT sdt(table);
                if (sdt.isValid()) {
                    processSDT(sdt);
                }
            }
            break;
        }

        case TID_PMT: {
            PMT pmt (table);
            if (pmt.isValid() && _service.hasId(pmt.service_id)) {
                processPMT (pmt);
            }
            break;
        }

        default: {
            break;
        }
    }
}


//----------------------------------------------------------------------------
//  This method processes a Service Description Table (SDT).
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::processSDT(SDT& sdt)
{
    // Look for the service by name
    uint16_t service_id;
    assert(_service.hasName());
    if (!sdt.findService(_service.getName(), service_id)) {
        tsp->error(u"service \"%s\" not found in SDT", {_service.getName()});
        _abort = true;
        return;
    }

    // Remember service id
    _service.setId (service_id);
    tsp->verbose(u"service id is 0x%X", {service_id});

    // No longer need to filter the SDT
    _demux.removePID(PID_SDT);

    // Now filter the PAT to get the PMT PID's
    _demux.addPID(PID_PAT);
}


//----------------------------------------------------------------------------
//  This method processes a Program Association Table (PAT).
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::processPAT (PAT& pat)
{
    // Register all PMT PID's as used.
    for (PAT::ServiceMap::const_iterator it = pat.pmts.begin(); it != pat.pmts.end(); ++it) {
        _input_pids.set(it->second);
    }

    // Search service in the PAT
    assert (_service.hasId());
    PAT::ServiceMap::const_iterator patit = pat.pmts.find (_service.getId());
    if (patit == pat.pmts.end()) {
        // Service not found, error
        tsp->error(u"service id %d (0x%X) not found in PAT", {_service.getId(), _service.getId()});
        _abort = true;
        return;
    }

    // If a previous PMT PID was known, no long filter it
    if (_service.hasPMTPID()) {
        _demux.removePID(_service.getPMTPID());
    }

    // Filter PMT PID
    _service.setPMTPID(patit->second);
    _demux.addPID(patit->second);

    // Set PID to PMT packetizer
    _pzer_pmt.setPID(patit->second);
}


//----------------------------------------------------------------------------
//  This method processes a Program Map Table (PMT).
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::processPMT(PMT& pmt)
{
    // Make sure this is the right service
    if (!_service.hasId(pmt.service_id)) {
        return;
    }

    // Collect all PIDS to scramble
    for (PMT::StreamMap::const_iterator it = pmt.streams.begin(); it != pmt.streams.end(); ++it) {
        const PID pid = it->first;
        const PMT::Stream& stream(it->second);
        _input_pids.set(pid);
        if ((_scramble_audio && stream.isAudio()) || (_scramble_video && stream.isVideo()) || (_scramble_subtitles && stream.isSubtitles())) {
            _scrambled_pids.set(pid);
            tsp->verbose(u"starting scrambling PID 0x%X", {pid});
        }
    }

    // Allocate a PID value for ECM if necessary
    if (!_use_fixed_key && _ecm_pid == PID_NULL) {
        // Start at service PMT PID, then look for an unused one.
        for (_ecm_pid = _service.getPMTPID() + 1; _ecm_pid < PID_NULL && _input_pids.test (_ecm_pid); _ecm_pid++) {}
        if (_ecm_pid >= PID_NULL) {
            tsp->error(u"cannot find an unused PID for ECM, try --pid-ecm");
            _abort = true;
        }
        else {
            tsp->verbose(u"using PID %d (0x%X) for ECM", {_ecm_pid, _ecm_pid});
        }
    }

    // With ECM generation, modify the PMT
    if (!_use_fixed_key) {

        // Create a CA_descriptor
        CADescriptor ca_desc ((_super_cas_id >> 16) & 0xFFFF, _ecm_pid);
        ca_desc.private_data = _ca_desc_private;

        // Add the CA_descriptor at program level or component level
        if (_component_level) {
            // Add a CA_descriptor in each scrambled component
            for (PMT::StreamMap::iterator it = pmt.streams.begin(); it != pmt.streams.end(); ++it) {
                if (_scrambled_pids.test(it->first)) {
                    it->second.descs.add(ca_desc);
                }
            }
        }
        else {
            // Add one single CA_descriptor at program level
            pmt.descs.add(ca_desc);
        }

        // Packetize the modified PMT
        _pzer_pmt.removeSections(TID_PMT, pmt.service_id);
        _pzer_pmt.addTable(pmt);
    }

    // We are now ready to scramble packets
    _ready = true;

    // Initialize crypto-period management
    if (!_use_fixed_key) {

        // We need to know the bitrate in order to schedule crypto-periods
        if (_ts_bitrate == 0) {
            tsp->error(u"unknown bitrate, cannot schedule crypto-periods");
            _abort = true;
            return;
        }

        // Insert current ECM packets as soon as possible.
        _pkt_insert_ecm = _packet_count;

        // Next crypto-period
        _pkt_change_cw = _packet_count + PacketDistance (_ts_bitrate, _cp_duration);

        // Next ECM may start before or after next crypto-period
        _pkt_change_ecm = _delay_start > 0 ?
            _pkt_change_cw + PacketDistance (_ts_bitrate, _delay_start) :
            _pkt_change_cw - PacketDistance (_ts_bitrate, _delay_start);
    }
}


//----------------------------------------------------------------------------
// Check if we are in degraded mode or if we enter degraded mode
//----------------------------------------------------------------------------

bool ts::ScramblerPlugin::inDegradedMode()
{
    if (_degraded_mode) {
        // Already in degraded mode, do not try to exit from it now.
        return true;
    }
    else if (nextECM().ecmReady()) {
        // Next ECM ready, no need to enter degraded mode.
        return false;
    }
    else {
        // Entering degraded mode
        tsp->warning(u"Next ECM not ready, entering degraded mode");
        return _degraded_mode = true;
    }
}


//----------------------------------------------------------------------------
// Try to exit from degraded mode
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::tryExitDegradedMode()
{
    // If not in degraded mode, nothing to do
    if (!_degraded_mode) {
        return;
    }

    // We are in degraded mode. If next ECM not yet ready, stay degraded
    if (!nextECM().ecmReady()) {
        return;
    }

    // Next ECM is ready, at last. Exit degraded mode.
    tsp->info(u"Next ECM ready, exiting from degraded mode");
    _degraded_mode = false;

    // Compute next CW and ECM change.
    if (_delay_start < 0) {
        // Start broadcasting ECM before beginning of crypto-period, ie. now
        changeECM();
        // Postpone CW change
        _pkt_change_cw = _packet_count + PacketDistance (_ts_bitrate, _delay_start);
    }
    else {
        // Change CW now.
        changeCW();
        // Start broadcasting ECM after beginning of crypto-period
        _pkt_change_ecm = _packet_count + PacketDistance(_ts_bitrate, _delay_start);
    }
}


//----------------------------------------------------------------------------
// Perform crypto-period transition, for CW or ECM
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::changeCW()
{
    // Allowed to change CW only if not in degraded mode
    if (!inDegradedMode()) {
        // Point to next crypto-period
        _current_cw = (_current_cw + 1) & 0x01;
        // Use new control word
        currentCW().initScramblerKey();
        // Determine new transition point
        _pkt_change_cw = _packet_count + PacketDistance (_ts_bitrate, _cp_duration);
        // Generate (or start generating) next ECM when using ECM(N) in cp(N)
        if (_current_ecm == _current_cw) {
            nextCW().initNext (currentCW());
        }
    }
}

void ts::ScramblerPlugin::changeECM()
{
    // Allowed to change CW only if not in degraded mode
    if (!inDegradedMode()) {
        // Point to next crypto-period
        _current_ecm = (_current_ecm + 1) & 0x01;
        // Determine new transition point
        _pkt_change_ecm = _packet_count + PacketDistance (_ts_bitrate, _cp_duration);
        // Generate (or start generating) next ECM when using ECM(N) in cp(N)
        if (_current_ecm == _current_cw) {
            nextCW().initNext (currentCW());
        }
    }
}


//----------------------------------------------------------------------------
// Packet processing method
//----------------------------------------------------------------------------

ts::ProcessorPlugin::Status ts::ScramblerPlugin::processPacket (TSPacket& pkt, bool& flush, bool& bitrate_changed)
{
    // Count packets
    _packet_count++;

    // Track all input PIDs
    const PID pid = pkt.getPID();
    _input_pids.set(pid);

    // Maintain bitrate, keep previous one if unknown
    {
        const BitRate br = tsp->bitrate();
        if (br != 0) {
            _ts_bitrate = br;
        }
    }

    // Filter interesting sections
    _demux.feedPacket(pkt);

    // If a fatal error occured during section analysis, give up.
    if (_abort) {
        return TSP_END;
    }

    // Abort if allocated PID for ECM is already present in TS
    if (_ecm_pid != PID_NULL && pid == _ecm_pid) {
        tsp->error(u"ECM PID allocation conflict, used 0x%X, now found as input PID, try another --pid-ecm", {pid});
        return TSP_END;
    }

    // While not ready to transmit, nullify all packets
    if (!_ready) {
        return TSP_NULL;
    }

    // Perform crypto-period management
    if (!_use_fixed_key) {

        // Packetize modified PMT when ECM generation is used
        if (pid == _pzer_pmt.getPID()) {
            _pzer_pmt.getNextPacket(pkt);
            return TSP_OK;
        }

        // Is it time to apply the next control word ?
        if (_packet_count >= _pkt_change_cw) {
            changeCW();
        }

        // Is it time to start broadcasting the next ECM ?
        if (_packet_count >= _pkt_change_ecm) {
            changeECM();
        }

        // Insert an ECM packet (replace a null packet) when time to do so
        if (pid == PID_NULL && _packet_count >= _pkt_insert_ecm) {

            // Compute next insertion point (approximate)
            assert(_ecm_bitrate != 0);
            _pkt_insert_ecm += BitRate(_ts_bitrate / _ecm_bitrate);

            // Exit degraded mode ?
            tryExitDegradedMode();

            // Replace current null packet with an ECM packet
            currentECM().getNextECMPacket (pkt);
            return TSP_OK;
        }
    }

    // If the packet has no payload or its PID is not to be scrambled, there is nothing to do.
    if (!pkt.hasPayload() || !_scrambled_pids.test (pid)) {
        return TSP_OK;
    }

    // If packet is already scrambled, error or ignore (do not modify packet)
    if (pkt.isScrambled()) {
        if (_ignore_scrambled) {
            if (!_conflict_pids.test (pid)) {
                tsp->verbose(u"found input scrambled packets in PID %d (0x%X), ignored", {pid, pid});
                _conflict_pids.set (pid);
            }
            return TSP_OK;
        }
        else {
            tsp->error(u"packet already scrambled in PID %d (0x%X)", {pid, pid});
            return TSP_END;
        }
    }

    // Manage partial scrambling
    if (_partial_clear > 0) {
        // Do not scramble this packet
        _partial_clear--;
        return TSP_OK;
    }
    else {
        // Scramble this packet and reinit subsequent number of packets to keep clear
        _partial_clear = _partial_scrambling - 1;
    }

    // Scramble the packet payload.
    _current_key.encrypt(pkt.getPayload(), pkt.getPayloadSize());
    _scrambled_count++;

    // Set scrambling_control_value in TS header.
    if (_use_fixed_key) {
        // With fixed key, use "even key" (there is only one key but we must set something).
        pkt.setScrambling(SC_EVEN_KEY);
    }
    else {
        pkt.setScrambling(currentCW().getScramblingControlValue());
    }

    return TSP_OK;
}


//----------------------------------------------------------------------------
// CryptoPeriod default constructor.
//----------------------------------------------------------------------------

ts::ScramblerPlugin::CryptoPeriod::CryptoPeriod() :
    _scrambler(0),
    _cp_number(0),
    _ecm_ok(false),
    _ecm(),
    _ecm_pkt_index(0),
    _cw_current(),
    _cw_next()
{
}


//----------------------------------------------------------------------------
// Initialize first crypto period.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::initCycle (ScramblerPlugin* scrambler, uint16_t cp_number)
{
    _scrambler = scrambler;
    _cp_number = cp_number;
    _scrambler->_cw_gen.read(_cw_current, sizeof(_cw_current));
    _scrambler->_cw_gen.read(_cw_next, sizeof(_cw_next));
    generateECM();
}


//----------------------------------------------------------------------------
// Initialize crypto period following specified one.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::initNext (const CryptoPeriod& previous)
{
    _scrambler = previous._scrambler;
    _cp_number = previous._cp_number + 1;
    ::memcpy(_cw_current, previous._cw_next, sizeof(_cw_current));  // Flawfinder: ignore: memcpy()
    _scrambler->_cw_gen.read(_cw_next, sizeof(_cw_next));
    generateECM();
}


//----------------------------------------------------------------------------
// Generate the ECM for a crypto-period.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::generateECM()
{
    _ecm_ok = false;

    if (_scrambler->_synchronous_ecmg) {
        // Synchronous ECM generation
        ecmgscs::ECMResponse response;
        if (!_scrambler->_ecmg.generateECM(_cp_number,
                                           _cw_current,
                                           _cw_next,
                                           _scrambler->_access_criteria.data(),
                                           _scrambler->_access_criteria.size(),
                                           uint16_t(_scrambler->_cp_duration / 100),
                                           response)) {
            // Error, message already reported
            _scrambler->_abort = true;
        }
        else {
            handleECM(response);
        }
    }
    else {
        // Asynchronous ECM generation
        if (!_scrambler->_ecmg.submitECM(_cp_number,
                                         _cw_current,
                                         _cw_next,
                                         _scrambler->_access_criteria.data(),
                                         _scrambler->_access_criteria.size(),
                                         uint16_t(_scrambler->_cp_duration / 100),
                                         this)) {
            // Error, message already reported
            _scrambler->_abort = true;
        }
    }
}


//----------------------------------------------------------------------------
// Invoked when an ECM is available, maybe in the context of an external thread
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::handleECM(const ecmgscs::ECMResponse& response)
{
    if (_scrambler->_channel_status.section_TSpkt_flag == 0) {
        // ECMG returns ECM in section format
        SectionPtr sp(new Section(response.ECM_datagram));
        if (!sp->isValid()) {
            _scrambler->tsp->error(u"ECMG returned an invalid ECM section (%d bytes)", {response.ECM_datagram.size()});
            _scrambler->_abort = true;
            return;
        }
        // Packetize the section
        OneShotPacketizer pzer(_scrambler->_ecm_pid, true);
        pzer.addSection(sp);
        pzer.getPackets(_ecm);

    }
    else if (response.ECM_datagram.size() % PKT_SIZE != 0) {
        // ECMG returns ECM in packet format, but not an integral number of packets
        _scrambler->tsp->error(u"invalid ECM size (%d bytes), not a multiple of %d", {response.ECM_datagram.size(), PKT_SIZE});
        _scrambler->_abort = true;
        return;
    }
    else {
        // ECMG returns ECM in packet format
        _ecm.resize(response.ECM_datagram.size() / PKT_SIZE);
        ::memcpy(&_ecm[0].b, response.ECM_datagram.data(), response.ECM_datagram.size());  // Flawfinder: ignore: memcpy()
    }

    _scrambler->tsp->debug(u"got ECM for crypto-period %d, %d packets", {_cp_number, _ecm.size()});

    _ecm_pkt_index = 0;

    // Last instruction: set the volatile boolean
    _ecm_ok = true;
}


//----------------------------------------------------------------------------
// Get next ECM packet
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::getNextECMPacket (TSPacket& pkt)
{
    if (!_ecm_ok || _ecm.size() == 0) {
        // No ECM, return a null packet
        pkt = NullPacket;
    }
    else {
        // Copy ECM packet
        assert(_ecm_pkt_index < _ecm.size());
        pkt = _ecm[_ecm_pkt_index];
        // Move to next ECM packet
        if (++_ecm_pkt_index >= _ecm.size()) {
            _ecm_pkt_index = 0;
        }
        // Adjust PID and continuity counter in TS packet
        pkt.setPID(_scrambler->_ecm_pid);
        pkt.setCC(_scrambler->_ecm_cc);
        _scrambler->_ecm_cc = (_scrambler->_ecm_cc + 1) & 0x0F;
    }
}


//----------------------------------------------------------------------------
// Initialize the scrambler with the current control word.
//----------------------------------------------------------------------------

void ts::ScramblerPlugin::CryptoPeriod::initScramblerKey() const
{
    _scrambler->tsp->debug(u"using new control word: " + UString::Dump(_cw_current, sizeof(_cw_current), UString::SINGLE_LINE));
    _scrambler->_current_key.init(_cw_current, _scrambler->_cw_mode);
}
