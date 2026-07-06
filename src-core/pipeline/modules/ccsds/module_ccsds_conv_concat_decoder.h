#pragma once

#include "common/codings/deframing/bpsk_ccsds_deframer.h"
#include "common/codings/reedsolomon/reedsolomon.h"
#include "common/codings/viterbi/viterbi_1_2.h"
#include "common/codings/viterbi/viterbi_punc.h"
#include "common/dsp/demod/constellation.h"
#include "common/dsp/utils/random.h"
#include "pipeline/modules/base/filestream_to_filestream.h"
#include <memory>
#include <string>
#include <vector>

namespace satdump
{
    namespace pipeline
    {
        namespace ccsds
        {
            /*
            This decoder is meant to decode convolutional r=2 k=7 codes
            concatenated with Reed-Solomon parity bits. This soft of FEC
            is pretty common on CCSDS-compliant satellites, with some
            varients such as :
                - Differential (NRZ-M) encoding
                - Different CADU size
                - RS 223 or 239 codes, with usually I=4 or I=5
                - Bit swap and 90 degs phase rotation on BPSK
                - Reed-Solomon lacking dual-basis

            All those variations are in the end pretty minor so a common
            decoder can be used instead allowing a high degree of tuning.

            Decoding is done by first locking a streaming viterbi decoder
            onto a specific state of the provided modulation, to then feed
            the decoded data to a deframer.

            It is recommended to use a rather low thresold for the Viterbi
            decoder, usually just below the average to ensure it locks as
            soon as possible. 0.300 seems to be good.

            The ASM Marker is left configurable as other satellites use
            similar protocols, just with a different syncword.

            CCSDS naming is kept mostly because this specific convolutional
            code is from the specification and most satellites will use
            CCSDS-compliant concatenated codings anyway.

            When conv_rate is set to "auto", all supported puncture rates
            are decoded in parallel every iteration. The active decoder is
            kept until it loses lock (hysteresis), at which point the locked
            decoder with the lowest BER takes over, allowing instant switching.
            */
            class CCSDSConvConcatDecoderModule : public base::FileStreamToFileStreamModule
            {
            protected:
                const bool is_ccsds; // Just to know if we should output .cadu or .frm

                const std::string d_constellation_str;     // Constellation type string
                dsp::constellation_type_t d_constellation; // Constellation type
                bool d_bpsk_90;                            // Special case for BPSK shifted by 90 degs + IQ-swapped
                bool d_oqpsk_mode;                         // OQPSK does NOT guarantee IQ stability
                const bool d_iq_invert;                    // For some QPSK sats, can need to be inverted...

                const int d_cadu_size;   // CADU Size in bits, including ASM
                const int d_cadu_bytes;  // CADU Size in bytes, including ASM
                const int d_buffer_size; // Processing buffer size, default half of a frame (= d_cadu_size)

                const int d_viterbi_outsync_after;
                const float d_viterbi_ber_threasold;

                // If > 0: force all viterbi decoders to outsync when deframer hasn't synced for this many seconds
                const double d_deframer_nosync_timeout;
                // Timer for deframer nosync detection.
                // A negative value means the system is in a post-reset cooldown period; the timeout
                // cannot fire again until the timer climbs back to 0 from its negative start.
                double d_deframer_nosync_timer = 0;
                // Consecutive forced-reset counter for exponential backoff calculation.
                int d_deframer_nosync_reset_count = 0;

                const bool d_diff_decode; // If NRZ-M Decoding is required or not

                const bool d_derand;          // Perform derandomizion or not
                const bool d_derand_after_rs; // Derandomization after RS
                const int d_derand_from;      // Byte to start derand on

                const std::string d_conv_type; // Conv rate identifier: "1/2", "2/3", "3/4", "5/6", "7/8", or "auto"

                const int d_rs_interleaving_depth; // RS Interleaving depth. If = 0, then RS is disabled
                const int d_rs_fill_bytes;         // RS Frame size, if -1, no puncturing
                const bool d_rs_dualbasis;         // RS Representation. Dual basis or none?
                const std::string d_rs_type;       // RS Type identifier
                const bool d_rs_usecheck;          // RS Used as frame check?

                enum vitrate_t
                {
                    PUNCRATE_1_2,
                    PUNCRATE_2_3,
                    PUNCRATE_3_4,
                    PUNCRATE_5_6,
                    PUNCRATE_7_8,
                };

                // Per-rate viterbi decoder slot, used for both fixed and parallel-auto pools
                struct ViterbiSlot
                {
                    std::shared_ptr<viterbi::Viterbi1_2>     v12; // non-null only for rate 1/2
                    std::shared_ptr<viterbi::Viterbi_Depunc>  vp; // non-null for rates 2/3 .. 7/8
                    vitrate_t rate;
                    std::string name;
                    uint8_t *out = nullptr; // owned output buffer (d_buffer_size * 8 bytes)
                    int last_vitout = 0;

                    int work(int8_t *input, int size)
                    {
                        last_vitout = v12 ? v12->work(input, size, out) : vp->work(input, size, out);
                        return last_vitout;
                    }
                    float ber() const { return v12 ? v12->ber() : vp->ber(); }
                    int getState() const { return v12 ? v12->getState() : vp->getState(); }
                    void reset() { if (v12) v12->reset(); else vp->reset(); }
                };

                bool d_auto_rate = false;             // true when conv_rate == "auto"
                std::vector<ViterbiSlot> d_rate_pool; // 1 slot (fixed rate) or 5 slots (auto)
                int d_active_rate_idx = 0;            // pool index currently feeding the deframer
                std::string viterbi_rate_str;         // display name of the active rate

                // Non-owning alias into d_rate_pool[d_active_rate_idx].out, updated every iteration
                uint8_t *viterbi_out = nullptr;
                int8_t *soft_buffer;
                uint8_t *frame_buffer;

                std::shared_ptr<deframing::BPSK_CCSDS_Deframer> deframer;
                std::shared_ptr<reedsolomon::ReedSolomon> reed_solomon;

                int errors[10];

                // UI Stuff
                float ber_history[200];
                dsp::Random rng;

                float viterbi_ber = 0;
                int viterbi_lock = 0;

            public:
                CCSDSConvConcatDecoderModule(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
                ~CCSDSConvConcatDecoderModule();
                void process();
                void drawUI(bool window);

                nlohmann::json getModuleStats();

            public:
                static std::string getID();
                virtual std::string getIDM() { return getID(); };
                static nlohmann::json getParams() { return {}; } // TODOREWORK
                static std::shared_ptr<ProcessingModule> getInstance(std::string input_file, std::string output_file_hint, nlohmann::json parameters);
            };
        } // namespace ccsds
    } // namespace pipeline
} // namespace satdump
