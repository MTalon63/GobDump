#include "detect_header.h"
#include "common/wav.h"
#include <cstdint>
#include "wav.h"
#ifdef BUILD_ZIQ
#include "ziq.h"
#endif

#include "logger.h"

#include "ziq2.h"
#include <cctype>
#include <regex>

HeaderInfo try_parse_header(std::string file)
{
    HeaderInfo info;

    if (wav::isValidWav(wav::parseHeaderFromFileWav(file)))
    {
        logger->debug("File is wav!");
        wav::WavHeader wav_header = wav::parseHeaderFromFileWav(file);
        info.samplerate = wav_header.samplerate;
        info.valid = true;
        if (wav_header.bits_per_sample == 8 && wav_header.audio_Format == 1)
            info.type = "cu8";
        else if (wav_header.bits_per_sample == 16 && wav_header.audio_Format == 1)
            info.type = "cs16";
        else if (wav_header.bits_per_sample == 32 && wav_header.audio_Format == 1)
            info.type = "cs32";
        else if (wav_header.bits_per_sample == 32 && wav_header.audio_Format == 3)
            info.type = "cf32";
        else
        {
            info.valid = false;
            logger->warn("Unsupported WAV format! Bits per sample: %hu, Format: %hu",
                wav_header.bits_per_sample, wav_header.audio_Format);
        }
    }
    else if (wav::isValidRF64(wav::parseHeaderFromFileWav(file)))
    {
        wav::RF64Header rf64_header = wav::parseHeaderFromFileRF64(file);
        logger->debug("File is RF64!");
        info.samplerate = rf64_header.samplerate;
        info.valid = true;
        if (rf64_header.bits_per_sample == 8 && rf64_header.audio_Format == 1)
            info.type = "cu8";
        else if (rf64_header.bits_per_sample == 16 && rf64_header.audio_Format == 1)
            info.type = "cs16";
        else if (rf64_header.bits_per_sample == 32 && rf64_header.audio_Format == 1)
            info.type = "cs32";
        else if (rf64_header.bits_per_sample == 32 && rf64_header.audio_Format == 3)
            info.type = "cf32";
        else
        {
            info.valid = false;
            logger->warn("Unsupported RF64 format! Bits per sample: %hu, Format: %hu",
                rf64_header.bits_per_sample, rf64_header.audio_Format);
        }
    }
#ifdef BUILD_ZIQ
    else if (ziq::isValidZIQ(file))
    {
        logger->debug("File is ZIQ!");
        info.type = "ziq";
        info.valid = true;
        info.samplerate = ziq::getCfgFromFile(file).samplerate;
    }
#endif
#ifdef BUILD_ZIQ2
    else if (ziq2::ziq2_is_valid_ziq2(file))
    {
        logger->debug("File is ZIQ2!");
        info.type = "ziq2";
        info.valid = true;
        info.samplerate = ziq2::ziq2_try_parse_header(file);
    }
#endif

    return info;
}

// A last-resort sample rate guess for raw captures whose names are not in any of
// the known formats above, such as "20230930_213309_FENGYUN 3F(dump)230M.s8".
// Without this such a file silently runs at the default 6 Msps, which trips the
// demodulator's SPS guard on any wideband link. Only a whole "230M"/"2.4M"/"6M"
// style token is accepted (the same notated form the UI's number widgets use);
// a frequency like "7500MHz" is rejected because the suffix is followed by a letter.
static uint64_t try_get_samplerate_from_name(std::string input_file)
{
    std::string stem = std::filesystem::path(input_file).stem().string();

    static const std::regex rate_regex(R"((?:^|[^0-9A-Za-z.])([0-9]+(?:\.[0-9]+)?)([kKmMgG])(?![A-Za-z]))");
    std::smatch match;
    if (!std::regex_search(stem, match, rate_regex))
        return 0;

    double value = std::stod(match[1].str());
    switch (std::toupper((unsigned char)match[2].str()[0]))
    {
    case 'K':
        value *= 1e3;
        break;
    case 'M':
        value *= 1e6;
        break;
    case 'G':
        value *= 1e9;
        break;
    }

    // Keep the guess inside a plausible SDR sample-rate range; a stray
    // frequency such as "2300M" in the name is not a capture rate.
    if (value < 1e3 || value > 2e9)
        return 0;

    return (uint64_t)value;
}

void try_get_params_from_input_file(nlohmann::json &parameters, std::string input_file)
{
    if (std::filesystem::exists(input_file) && !std::filesystem::is_directory(input_file))
    {
        HeaderInfo hdr = try_parse_header(input_file);
        if (hdr.valid)
        {
            if (!parameters.contains("samplerate"))
                parameters["samplerate"] = hdr.samplerate;
            if (!parameters.contains("baseband_format"))
                parameters["baseband_format"] = hdr.type;
        }
        wav::FileMetadata md = wav::tryParseFilenameMetadata(input_file, false);
        if (!parameters.contains("start_timestamp") && md.timestamp != 0)
            parameters["start_timestamp"] = md.timestamp;
        if (!parameters.contains("samplerate") && md.samplerate != 0)
            parameters["samplerate"] = md.samplerate;
        if (!parameters.contains("frequency") && md.frequency != 0)
            parameters["frequency"] = md.frequency;
        if (!parameters.contains("baseband_format") && md.baseband_format != "")
            parameters["baseband_format"] = md.baseband_format;

        if (!parameters.contains("samplerate"))
            if (uint64_t samplerate = try_get_samplerate_from_name(input_file); samplerate != 0)
                parameters["samplerate"] = samplerate;
    }
}