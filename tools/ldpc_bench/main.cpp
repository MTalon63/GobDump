#include "common/codings/ldpc/ccsds_ldpc.h"
#include "common/codings/ldpc/ldpc_decoder.h"
#include "common/codings/ldpc/make_ccsds.h"
#include "common/codings/ldpc/matrix/sparse_matrix.h"
#include "common/dsp/complex.h"
#include "common/dsp/utils/snr_estimator.h"
#include "core/plugin.h"
#include "logger.h"

#ifdef LDPC_BENCH_HAVE_AVX2
#include "ldpc_decoder/ldpc_decoder_avx.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants / configuration
// ---------------------------------------------------------------------------

static constexpr int R78_CODEWORD = 8176; // codeword (N)
static constexpr int R78_M = 1022;        // parity bits (M)
static constexpr int R78_K = 7154;        // info cols incl 18-zero prefix
static constexpr int R78_PREFIX = 18;     // punctured all-zero prefix; offset 18, data at [18..7153]
static constexpr int R78_DATA = 7136;     // true info bits per frame
static constexpr int R78_FRAME = 8160;    // transmitted frame (8176 -> 8160)

static constexpr int AGC_AMPLITUDE = 50; // Module AGC assumption (A)

struct BenchConfig
{
    std::vector<float> snr_db = {2, 3, 4, 5, 6};
    std::vector<std::string> algos = {"layered_normalized_min_sum", "normalized_min_sum", "self_corrected_min_sum"};
    std::vector<std::string> scaling = {"calibrated", "heuristic"};
    int frames = 200;
    int cap_iterations = 50;
    std::string out_file = "";
    bool generic = false;
    float scale_mult = 1.0f; // factor applied to llr_scale
    uint32_t base_seed = 0x1D2E3F4A;
    // TEMP-DIAG: dump frozen inputs for the reference decoder.
    std::string dump_frames_dir = "";
};

// ---------------------------------------------------------------------------
// Small CLI parsing
// ---------------------------------------------------------------------------

static std::vector<std::string> split_commas(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == ',')
        {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

static std::vector<float> parse_float_list(const std::string &s)
{
    std::vector<float> out;
    for (auto &tok : split_commas(s))
        out.push_back(std::stof(tok));
    return out;
}

// ---------------------------------------------------------------------------
// Deterministic PRNG helpers
// ---------------------------------------------------------------------------

static uint32_t xorshift32(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// ---------------------------------------------------------------------------
// GF(2) Gauss-Jordan inversion and matrix-vector product (dense, M x M)
// ---------------------------------------------------------------------------

// Invert n x n GF(2) matrix; false if singular. Inverse read from right half of [A | I].
static bool invert_gf2(const std::vector<std::vector<uint8_t>> &A, std::vector<std::vector<uint8_t>> &out)
{
    int n = (int)A.size();
    std::vector<std::vector<uint8_t>> aug(n, std::vector<uint8_t>(2 * n, 0));
    for (int i = 0; i < n; i++)
    {
        for (int j = 0; j < n; j++)
            aug[i][j] = A[i][j];
        aug[i][n + i] = 1;
    }

    int rank = 0;
    for (int c = 0; c < n && rank < n; c++)
    {
        int piv = -1;
        for (int i = rank; i < n; i++)
        {
            if (aug[i][c])
            {
                piv = i;
                break;
            }
        }
        if (piv < 0)
            continue;

        std::swap(aug[rank], aug[piv]);
        for (int i = 0; i < n; i++)
        {
            if (i != rank && aug[i][c])
                for (int j = 0; j < 2 * n; j++)
                    aug[i][j] ^= aug[rank][j];
        }
        rank++;
    }

    if (rank != n)
        return false;

    out.assign(n, std::vector<uint8_t>(n, 0));
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            out[i][j] = aug[i][n + j];
    return true;
}

static void gf2_matvec(const std::vector<std::vector<uint8_t>> &M, const std::vector<uint8_t> &v, std::vector<uint8_t> &out)
{
    int n = (int)M.size();
    out.assign(n, 0);
    for (int i = 0; i < n; i++)
    {
        uint8_t acc = 0;
        for (int j = 0; j < n; j++)
            if (M[i][j] && v[j])
                acc ^= 1;
        out[i] = acc;
    }
}

// ---------------------------------------------------------------------------
// Systematic GF(2) encoder for the CCSDS (8176,7154) rate-7/8 code.
// H2 is rank-deficient (rank 1020), so H2*p = s is solved via RREF.
// ---------------------------------------------------------------------------

struct R78Encoder
{
    codings::ldpc::Sparse_matrix H;
    std::vector<std::vector<uint8_t>> h2_pivot_comb; // 1022 x 1022 RREF comb rows
    std::vector<int> h2_pivot_col;                   // pivot column per row (-1 if rank row)
    int h2_rank;

    R78Encoder()
    {
        H = codings::ldpc::ccsds_78::make_r78_code();
        build();
    }

    void build()
    {
        // Dense H2 (check rows x parity cols [7154..8175]).
        std::vector<std::vector<uint8_t>> H2(R78_M, std::vector<uint8_t>(R78_M, 0));
        for (int r = 0; r < R78_M; r++)
        {
            for (uint32_t col : H.get_cols_from_row(r))
            {
                if ((int)col >= R78_K)
                    H2[r][col - R78_K] = 1;
            }
        }

        // RREF(H2) with row-combination tracker to solve H2*p = s.
        std::vector<std::vector<uint8_t>> aug = H2;
        std::vector<std::vector<uint8_t>> comb(R78_M, std::vector<uint8_t>(R78_M, 0));
        for (int i = 0; i < R78_M; i++)
            comb[i][i] = 1;

        h2_pivot_col.assign(R78_M, -1);
        int rank = 0;
        for (int c = 0; c < R78_M && rank < R78_M; c++)
        {
            int piv = -1;
            for (int i = rank; i < R78_M; i++)
            {
                if (aug[i][c])
                {
                    piv = i;
                    break;
                }
            }
            if (piv < 0)
                continue;

            std::swap(aug[rank], aug[piv]);
            std::swap(comb[rank], comb[piv]);
            for (int i = 0; i < R78_M; i++)
            {
                if (i != rank && aug[i][c])
                {
                    for (int j = 0; j < R78_M; j++)
                    {
                        aug[i][j] ^= aug[rank][j];
                        comb[i][j] ^= comb[rank][j];
                    }
                }
            }
            h2_pivot_col[rank] = c;
            rank++;
        }
        h2_rank = rank;
        h2_pivot_comb = comb;
    }

    // Encode 7136 data bits into a full 8176-bit systematic codeword.
    std::vector<uint8_t> encode(const std::vector<uint8_t> &data) const
    {
        if ((int)data.size() != R78_DATA)
        {
            fprintf(stderr, "LDPC BENCH: FATAL, encode() expects %d data bits, got %zu.\n", R78_DATA, data.size());
            exit(1);
        }

        std::vector<uint8_t> m(R78_K, 0);
        for (int i = 0; i < R78_DATA; i++)
            m[R78_PREFIX + i] = data[i];

        // s = H1 * m.
        std::vector<uint8_t> s(R78_M, 0);
        for (int r = 0; r < R78_M; r++)
        {
            uint8_t acc = 0;
            for (uint32_t col : H.get_cols_from_row(r))
                if ((int)col < R78_K && m[col])
                    acc ^= 1;
            s[r] = acc;
        }

        // Solve H2*p = s via the RREF row-combination tracker.
        std::vector<uint8_t> p(R78_M, 0);
        for (int i = 0; i < h2_rank; i++)
        {
            uint8_t acc = 0;
            for (int j = 0; j < R78_M; j++)
                if (h2_pivot_comb[i][j] && s[j])
                    acc ^= 1;
            p[h2_pivot_col[i]] = acc;
        }
        for (int i = h2_rank; i < R78_M; i++)
        {
            uint8_t acc = 0;
            for (int j = 0; j < R78_M; j++)
                if (h2_pivot_comb[i][j] && s[j])
                    acc ^= 1;
            if (acc)
            {
                fprintf(stderr, "LDPC BENCH: FATAL, H2*p=s inconsistent in row %d.\n", i);
                exit(1);
            }
        }

        // c = [ m | p ]
        std::vector<uint8_t> c(R78_CODEWORD, 0);
        for (int i = 0; i < R78_K; i++)
            c[i] = m[i];
        for (int i = 0; i < R78_M; i++)
            c[R78_K + i] = p[i];

        if (!check_codeword(c))
        {
            fprintf(stderr, "LDPC BENCH: FATAL, generated codeword fails H*c==0! Encoder is inconsistent.\n");
            exit(1);
        }
        return c;
    }

    // Verify H*c == 0 over GF(2).
    bool check_codeword(const std::vector<uint8_t> &c) const
    {
        for (int r = 0; r < R78_M; r++)
        {
            uint8_t acc = 0;
            for (uint32_t col : H.get_cols_from_row(r))
                acc ^= c[col];
            if (acc)
                return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// AWGN + QPSK int8 soft generation: bit 0 -> -A, bit 1 -> +A, sigma = A/sqrt(SNR).
// ---------------------------------------------------------------------------

static void generate_soft_frame(const std::vector<uint8_t> &codeword, float snr_db, uint32_t seed, std::vector<int8_t> &soft_out)
{
    float db = snr_db;
    float linear = powf(10.0f, db / 10.0f);
    float sigma = (float)AGC_AMPLITUDE / sqrtf(linear);

    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, sigma);

    std::vector<int8_t> frame(R78_FRAME, 0);
    // 2 trailing bits [8158..8159] not read by decode(); frame tail not part of codeword.
    for (int i = 0; i < 8158; i++)
    {
        int bit = codeword[R78_PREFIX + i];
        float v = (bit ? (float)AGC_AMPLITUDE : -(float)AGC_AMPLITUDE) + dist(rng);
        frame[i] = (int8_t)std::clamp((int)llroundf(v), -127, 127);
    }
    frame[8158] = 0;
    frame[8159] = 0;

    soft_out = frame;
}

// Feed int8 soft as interleaved I/Q pairs (/127) to SNR estimators, mirroring the module.
static int feed_snr_estimators(const std::vector<int8_t> &soft, M2M4SNREstimator &m2m4, EVMSNREstimator &evm)
{
    int ncomp = R78_FRAME / 2; // QPSK: pairs of samples
    for (int i = 0; i < ncomp; i++)
    {
        complex_t s(soft[i * 2 + 0] / 127.0f, soft[i * 2 + 1] / 127.0f);
        complex_t buf[1] = {s};
        m2m4.update(buf, 1);
        evm.update(buf, 1);
    }
    return ncomp;
}

// LLR scaling factor mirroring the module ("calibrated" / "heuristic").
static float compute_llr_scale(bool calibrated, float evm_snr, float evm_signal, float evm_noise)
{
    if (calibrated)
    {
        // 0.5*sqrt(sig_pow/noi_pow) is gain-invariant scaling; clamp [0.25, 8.0].
        float sig_pow = powf(10.0f, evm_signal / 10.0f);
        float noi_pow = powf(10.0f, evm_noise / 10.0f);
        float scale = 0.5f * std::sqrt(sig_pow / (noi_pow + 1e-6f));
        return std::clamp(scale, 0.25f, 8.0f);
    }
    else
    {
        // heuristic: npwr = 2*10^(-SNR/20), scale = 1/npwr, clamp [0.25, 8.0].
        float npwr = 2.0f * powf(10.0f, -evm_snr / 20.0f);
        return std::clamp(1.0f / npwr, 0.25f, 8.0f);
    }
}

// Apply 256-entry scaled LUT (clamp to +/-127 int8).
static void apply_llr_scale(std::vector<int8_t> &soft, float llr_scale)
{
    int8_t lut[256];
    for (int v = -128; v < 128; v++)
        lut[v + 128] = (int8_t)std::clamp((long long)llroundf(v * llr_scale), -127LL, 127LL);
    for (auto &s : soft)
        s = lut[(uint8_t)(s + 128)];
}

// ---------------------------------------------------------------------------
// Decoder harness: runs one cell (snr, algo, scaling, path) over `frames`.
// Generic decodes per-frame; AVX2 decodes d_simd frames per call (batch metrics).
// ---------------------------------------------------------------------------

struct CellResult
{
    double converged_fraction;
    double mean_iters;
    double max_iters;
    double mean_diff;
    double mean_snr_m2m4;
    double mean_mer_evm;
    double mean_llr_scale;
    double mean_abs_llr; // effective post-scale mean |LLR| over the frame
    int simd_used;
};

static CellResult run_cell(const R78Encoder &encoder, codings::ldpc::CCSDSLDPC &dec,
                           const BenchConfig &cfg, float snr_db, bool calibrated)
{
    int simd = dec.simd();
    int cap = cfg.cap_iterations;

    std::vector<int8_t> inbuf(simd * R78_FRAME);
    std::vector<uint8_t> outbuf(simd * R78_FRAME);
    std::vector<std::vector<uint8_t>> known_info(simd, std::vector<uint8_t>(R78_DATA));

    int total_batches = (cfg.frames + simd - 1) / simd;
    int counted_frames = 0;
    uint64_t cum_conv = 0;
    double sum_iters = 0, max_iters = 0;
    uint64_t cum_diff = 0;
    double sum_m2m4 = 0, sum_mer = 0, sum_scale = 0, sum_abs = 0;

    for (int b = 0; b < total_batches; b++)
    {
        int base = b * simd;
        std::vector<float> frame_scale(simd, 1.0f);
        std::vector<float> frame_m2m4(simd, 0.0f);
        std::vector<float> frame_mer(simd, 0.0f);
        std::vector<float> frame_abs(simd, 0.0f);

        for (int lane = 0; lane < simd; lane++)
        {
            int frame_idx = base + lane;
            uint32_t seed = cfg.base_seed + (uint32_t)frame_idx * 0x9E3779B1u;

            // Deterministic data bits for this frame.
            std::vector<uint8_t> data(R78_DATA);
            uint32_t dseed = seed ^ 0xA5A5A5A5u;
            for (int i = 0; i < R78_DATA; i++)
                data[i] = (uint8_t)(xorshift32(dseed) & 1u);

            std::vector<uint8_t> codeword = encoder.encode(data);

            // Raw soft (pre-scaling) for the estimators.
            std::vector<int8_t> soft;
            generate_soft_frame(codeword, snr_db, seed, soft);

            M2M4SNREstimator m2m4(0.001f);
            EVMSNREstimator evm(4, 0.001f);
            feed_snr_estimators(soft, m2m4, evm);
            frame_m2m4[lane] = m2m4.snr();
            frame_mer[lane] = evm.snr();

            // LLR scaling (mirrors module) * scale_mult, applied before LUT.
            float scale = cfg.scale_mult * compute_llr_scale(calibrated, evm.snr(), evm.signal(), evm.noise());
            frame_scale[lane] = scale;
            apply_llr_scale(soft, scale);

            int64_t abs_sum = 0;
            for (int i = 0; i < R78_FRAME; i++)
                abs_sum += std::abs((int)soft[i]);
            frame_abs[lane] = (double)abs_sum / (double)R78_FRAME;

            std::memcpy(&inbuf[lane * R78_FRAME], soft.data(), R78_FRAME);
            known_info[lane] = data;

            // TEMP-DIAG: dump frozen post-scale softs + true codeword for the reference decoder.
            if (!cfg.dump_frames_dir.empty())
            {
                char dpath[512];
                snprintf(dpath, sizeof(dpath), "%s/frame_%d.bin", cfg.dump_frames_dir.c_str(), frame_idx);
                FILE *dfp = fopen(dpath, "wb");
                if (dfp)
                {
                    fwrite(soft.data(), 1, R78_FRAME, dfp);             // 8160 int8 softs
                    fwrite(codeword.data(), 1, R78_CODEWORD, dfp);       // 8176 true codeword
                    fclose(dfp);
                }
            }
        }

        dec.decode(inbuf.data(), outbuf.data(), cap);
        bool batch_converged = dec.converged();
        int batch_iters = dec.last_iterations();

        for (int lane = 0; lane < simd; lane++)
        {
            int frame_idx = base + lane;
            if (frame_idx >= cfg.frames)
                continue;

            counted_frames++;

            uint64_t diff = 0;
            for (int i = 0; i < R78_DATA; i++)
            {
                if (outbuf[lane * R78_FRAME + i] != known_info[lane][i])
                    diff++;
            }
            cum_diff += diff;

            if (simd == 1)
            {
                if (dec.converged())
                    cum_conv++;
                sum_iters += dec.last_iterations();
                max_iters = std::max(max_iters, (double)dec.last_iterations());
            }
            else
            {
                if (batch_converged)
                    cum_conv++;
                sum_iters += batch_iters;
                max_iters = std::max(max_iters, (double)batch_iters);
            }

            sum_m2m4 += frame_m2m4[lane];
            sum_mer += frame_mer[lane];
            sum_scale += frame_scale[lane];
            sum_abs += frame_abs[lane];
        }
    }

    CellResult res;
    res.simd_used = simd;
    res.converged_fraction = counted_frames ? (double)cum_conv / (double)counted_frames : 0.0;
    res.mean_iters = counted_frames ? sum_iters / (double)counted_frames : 0.0;
    res.max_iters = max_iters;
    res.mean_diff = counted_frames ? (double)cum_diff / (double)counted_frames : 0.0;
    res.mean_snr_m2m4 = counted_frames ? sum_m2m4 / (double)counted_frames : 0.0;
    res.mean_mer_evm = counted_frames ? sum_mer / (double)counted_frames : 0.0;
    res.mean_llr_scale = counted_frames ? sum_scale / (double)counted_frames : 0.0;
    res.mean_abs_llr = counted_frames ? sum_abs / (double)counted_frames : 0.0;
    return res;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

// High-SNR (10 dB) round-trip; if it fails while the encoder check passes, bug is SNR-independent.
static void diagnostic_high_snr_roundtrip(const R78Encoder &encoder, const BenchConfig &cfg)
{
    codings::ldpc::CCSDSLDPC dec(codings::ldpc::RATE_7_8, 0);
    dec.set_algorithm(codings::ldpc::ldpc_algorithm_from_string("layered_normalized_min_sum"));
    dec.set_early_termination(true);
    int simd = dec.simd();

    std::vector<int8_t> inbuf(simd * R78_FRAME);
    std::vector<uint8_t> outbuf(simd * R78_FRAME);

    uint64_t cum_diff = 0;
    int nframes = simd;
    std::vector<std::vector<uint8_t>> known_info(simd, std::vector<uint8_t>(R78_DATA));

    for (int lane = 0; lane < simd; lane++)
    {
        uint32_t seed = 0xF00D0000u + (uint32_t)lane;
        std::vector<uint8_t> data(R78_DATA);
        uint32_t dseed = seed ^ 0xA5A5A5A5u;
        for (int i = 0; i < R78_DATA; i++)
            data[i] = (uint8_t)(xorshift32(dseed) & 1u);
        std::vector<uint8_t> cw = encoder.encode(data);
        std::vector<int8_t> soft;
        // At 10 dB Es/N0 the raw +/-A symbols are strong; skip LLR scaling.
        generate_soft_frame(cw, 10.0f, seed, soft);
        std::memcpy(&inbuf[lane * R78_FRAME], soft.data(), R78_FRAME);
        known_info[lane] = data;
    }

    dec.decode(inbuf.data(), outbuf.data(), cfg.cap_iterations);
    bool conv = dec.converged();
    for (int lane = 0; lane < simd; lane++)
        for (int i = 0; i < R78_DATA; i++)
            if (outbuf[lane * R78_FRAME + i] != known_info[lane][i])
                cum_diff++;

    printf("\n[DIAG] high-SNR roundtrip @10dB layered_nms (simd=%d): converged=%s mean_diff=%.2f bits"
           " (encoder sanity OK => SNR-independent if failing)\n",
           simd, conv ? "YES" : "NO", (double)cum_diff / (double)nframes);
}

// Post-scale |LLR| histogram at 4 dB over [0,127], 20 bins.
static void diagnostic_llr_histogram(const R78Encoder &encoder, const BenchConfig &cfg)
{
    codings::ldpc::CCSDSLDPC dec(codings::ldpc::RATE_7_8, 0);
    dec.set_algorithm(codings::ldpc::ldpc_algorithm_from_string("layered_normalized_min_sum"));
    dec.set_early_termination(true);
    int simd = dec.simd();

    std::vector<int> hist(20, 0);
    long long total = 0;
    float mn = 1e9f, mx = -1e9f, s = 0;
    int nsamp = 0;

    for (int rep = 0; rep < std::min(cfg.frames, 16); rep++)
    {
        for (int lane = 0; lane < simd; lane++)
        {
            uint32_t seed = 0x41414141u + (uint32_t)rep * simd + (uint32_t)lane;
            std::vector<uint8_t> data(R78_DATA);
            uint32_t dseed = seed ^ 0xA5A5A5A5u;
            for (int i = 0; i < R78_DATA; i++)
                data[i] = (uint8_t)(xorshift32(dseed) & 1u);
            std::vector<uint8_t> cw = encoder.encode(data);
            std::vector<int8_t> soft;
            generate_soft_frame(cw, 4.0f, seed, soft);

            EVMSNREstimator evm(4, 0.001f);
            M2M4SNREstimator m2m4(0.001f);
            feed_snr_estimators(soft, m2m4, evm);
            float scale = compute_llr_scale(true, evm.snr(), evm.signal(), evm.noise());
            apply_llr_scale(soft, scale);

            for (int i = 0; i < R78_FRAME; i++)
            {
                int a = std::abs((int)soft[i]);
                if (a > mx) mx = (float)a;
                if (a < mn) mn = (float)a;
                s += (float)a;
                nsamp++;
                int bin = a * 20 / 128; // [0,127] -> 20 bins
                if (bin >= 20) bin = 19;
                hist[bin]++;
            }
            total++;
        }
    }

    printf("\n[DIAG] post-scale |LLR| @4dB histogram over [0,127]: min=%.1f mean=%.2f max=%.1f bins=[",
           mn, nsamp ? s / (float)nsamp : 0.0f, mx);
    for (int i = 0; i < 20; i++)
        printf("%d%s", hist[i], (i == 19) ? "" : ",");
    printf("]\n");
}

// Flag the layered kernel if flooding works but layered fails at 4 dB.
static void diagnostic_layered_vs_flooding(const R78Encoder &encoder, const BenchConfig &cfg)
{
    auto run = [&](const std::string &algo) {
        codings::ldpc::CCSDSLDPC dec(codings::ldpc::RATE_7_8, 0);
        dec.set_algorithm(codings::ldpc::ldpc_algorithm_from_string(algo));
        dec.set_early_termination(true);
        BenchConfig c2 = cfg;
        c2.frames = std::min(cfg.frames, 64);
        CellResult r = run_cell(encoder, dec, c2, 4.0f, true);
        return r;
    };

    CellResult lay = run("layered_normalized_min_sum");
    CellResult flood = run("normalized_min_sum");
    printf("[DIAG] 4dB layered_nms conv=%.3f vs flooding_nms conv=%.3f => %s\n",
           lay.converged_fraction, flood.converged_fraction,
           (lay.converged_fraction + 0.05) < flood.converged_fraction ? "LAYERED KERNEL SUSPECT" : "OK");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void print_usage(const char *name)
{
    printf(
        "Usage: %s [options]\n"
        "  --snr 2,3,4,5,6            Es/N0 sweep in dB\n"
        "  --algos a,b,c              algorithms to sweep\n"
        "  --scaling calibrated,heuristic   LLR scaling to sweep\n"
        "  --frames N                 frames per cell (default 200)\n"
        "  --cap-iterations N         decode iteration cap (default 50)\n"
        "  --generic                  force the scalar (generic) decoder path\n"
        "  --scale-mult X             multiply computed llr_scale by X before LUT (default 1.0)\n"
        "  --dump-frames DIR          TEMP-DIAG: write frozen post-scale softs + codeword per frame\n"
        "  --out file.csv             write CSV to file (default stdout)\n"
        , name);
}

int main(int argc, char *argv[])
{
    initLogger();
    completeLoggerInit();

    BenchConfig cfg;

    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto take = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--snr")
            cfg.snr_db = parse_float_list(take());
        else if (a == "--algos")
            cfg.algos = split_commas(take());
        else if (a == "--scaling")
            cfg.scaling = split_commas(take());
        else if (a == "--frames")
            cfg.frames = std::stoi(take());
        else if (a == "--cap-iterations")
            cfg.cap_iterations = std::stoi(take());
        else if (a == "--out")
            cfg.out_file = take();
        else if (a == "--generic")
            cfg.generic = true;
        else if (a == "--scale-mult")
            cfg.scale_mult = std::stof(take());
        else if (a == "--dump-frames")
            cfg.dump_frames_dir = take();
        else if (a == "--help" || a == "-h")
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            fprintf(stderr, "Unknown arg: %s\n", a.c_str());
            print_usage(argv[0]);
            return 1;
        }
    }

    // Register the AVX2 decoder so the 16-lane path works without a plugin scan.
#ifdef LDPC_BENCH_HAVE_AVX2
    satdump::eventBus->register_handler<codings::ldpc::GetLDPCDecodersEvent>(
        [](codings::ldpc::GetLDPCDecodersEvent evt) {
            evt.decoder_list.insert({"avx2", [](codings::ldpc::Sparse_matrix pcm) {
                                         return (codings::ldpc::LDPCDecoder *)new codings::ldpc::LDPCDecoderAVX(pcm);
                                     }});
        });
#endif

    // Force the scalar path via the env knob the codebase honours.
    if (cfg.generic)
    {
#ifdef _WIN32
        _putenv_s("GOBDUMP_LDPC_GENERIC", "1");
#else
        setenv("GOBDUMP_LDPC_GENERIC", "1", 1);
#endif
    }

    R78Encoder encoder;

    std::string decoder_path = cfg.generic ? "generic" : "avx2";

    // Open CSV (stdout or file).
    std::ostream *out = &std::cout;
    std::ofstream ofs;
    if (!cfg.out_file.empty())
    {
        ofs.open(cfg.out_file);
        if (!ofs.is_open())
        {
            fprintf(stderr, "LDPC BENCH: failed to open %s\n", cfg.out_file.c_str());
            return 1;
        }
        out = &ofs;
    }

    *out << "snr,algo,scaling,decoder_path,converged_fraction,mean_iters,max_iters,mean_diff,mean_snr_m2m4,mean_mer_evm,mean_llr_scale,mean_abs_llr\n";

    for (float snr : cfg.snr_db)
    {
        for (const std::string &algo : cfg.algos)
        {
            for (const std::string &scaling : cfg.scaling)
            {
                codings::ldpc::CCSDSLDPC dec(codings::ldpc::RATE_7_8, 0);
                dec.set_algorithm(codings::ldpc::ldpc_algorithm_from_string(algo));
                dec.set_early_termination(true);

                CellResult r = run_cell(encoder, dec, cfg, snr, scaling == "calibrated");

                char line[512];
                snprintf(line, sizeof(line), "%.1f,%s,%s,%s,%.4f,%.2f,%.2f,%.4f,%.2f,%.2f,%.4f,%.2f",
                         snr, algo.c_str(), scaling.c_str(), decoder_path.c_str(),
                         r.converged_fraction, r.mean_iters, r.max_iters, r.mean_diff,
                         r.mean_snr_m2m4, r.mean_mer_evm, r.mean_llr_scale, r.mean_abs_llr);
                *out << line << "\n";

                printf("  [%s] snr=%4.1f dB alg=%s scale=%s simd=%d conv=%.3f iters=%.1f diff=%.2f\n",
                       decoder_path.c_str(), snr, algo.c_str(), scaling.c_str(), r.simd_used,
                       r.converged_fraction, r.mean_iters, r.mean_diff);
            }
        }
    }

    if (ofs.is_open())
        ofs.close();

    printf("\nLDPC BENCH done. Decoder path: %s\n", decoder_path.c_str());
    return 0;
}