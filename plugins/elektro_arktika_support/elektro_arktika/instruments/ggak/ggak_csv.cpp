#include "ggak_csv.h"
#include "logger.h"
#include "utils/time.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <locale>

namespace elektro_arktika
{
    namespace ggak
    {
        namespace
        {
            constexpr const char *FRAME_HEADER =
                "host_utc,elapsed_s,asm_hex,frame_type,frame_type_name,master_counter,channel_counter,"
                "source_id,coarse_time,fine_time,checksum_ok,checksum_raw,payload_hex";

            constexpr const char *MAG_HEADER =
                "host_utc,elapsed_s,master_counter,record_index,B_total_nT,Bx_nT,By_nT,Bz_nT,voltage_V,"
                "B_total_raw,Bx_raw,By_raw,Bz_raw,status_flag,valid,checksum_ok";

            constexpr const char *PARTICLE_HEADER =
                "host_utc,elapsed_s,master_counter,record_index,sub_type,Ep600,Ep800,Ep1100,"
                "Cg1,Cg2,Cg3,Cg4,MIP,checksum_ok";

            constexpr int SKL_MAX_BINS = 206;

            constexpr const char *HK_HEADER =
                "host_utc,elapsed_s,master_counter,record_type,record_index,seq,ch0,ch1,ch2,ch3,ch4,checksum_ok";

            std::string csv_escape(const std::string &s)
            {
                if (s.find_first_of(",\"\r\n") == std::string::npos)
                    return s;

                std::string out;
                out.reserve(s.size() + 2);
                out.push_back('"');
                for (char c : s)
                {
                    if (c == '"')
                        out += "\"\"";
                    else
                        out.push_back(c);
                }
                out.push_back('"');
                return out;
            }

            std::string hex_encode(const uint8_t *data, size_t len)
            {
                static const char hex[] = "0123456789abcdef";
                std::string out;
                out.resize(len * 2);
                for (size_t i = 0; i < len; i++)
                {
                    out[i * 2] = hex[data[i] >> 4];
                    out[i * 2 + 1] = hex[data[i] & 0x0F];
                }
                return out;
            }

            void now_stamp(std::string &iso, std::string &day)
            {
                std::time_t t = static_cast<std::time_t>(satdump::getTime());
                std::tm tm{};
#ifdef _WIN32
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif

                char iso_buf[32];
                std::snprintf(iso_buf, sizeof(iso_buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min, tm.tm_sec);
                iso = iso_buf;

                char day_buf[16];
                std::snprintf(day_buf, sizeof(day_buf), "%04d-%02d-%02d",
                              tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
                day = day_buf;
            }

            bool file_header_matches(const std::string &path, const std::string &header)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in.is_open())
                    return false;

                std::string line;
                if (!std::getline(in, line))
                    return true; // empty file, header will be written

                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line == header;
            }

            bool file_is_empty(const std::string &path)
            {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                    return true;
                auto size = std::filesystem::file_size(path, ec);
                return ec ? true : size == 0;
            }

            std::string esa_column_name(int idx)
            {
                static const char *energies[12] = {
                    "e0_05keV", "e0_25keV", "e0_45keV", "e0_80keV",
                    "e1_30keV", "e2_50keV", "e4_50keV", "e7_50keV",
                    "e10_00keV", "e13_00keV", "e16_00keV", "e20_00keV"};
                if (idx >= 0 && idx < 12)
                    return energies[idx];
                return "bin" + std::to_string(idx + 1);
            }
        } // namespace

        GGAKCSVWriter::~GGAKCSVWriter() { close(); }

        bool GGAKCSVWriter::open(const std::string &directory_, const std::string &prefix_,
                                 uint64_t max_bytes_, bool write_raw_frames)
        {
            close();

            directory = directory_;
            prefix = prefix_;
            max_bytes = max_bytes_;

            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (!std::filesystem::is_directory(directory))
                return false;

            initStreams();
            streams[S_FRAMES].enabled = write_raw_frames;
            for (int i = S_MAG; i < S_COUNT; i++)
                streams[i].enabled = true;

            bool any_enabled = false;
            for (const auto &s : streams)
                if (s.enabled)
                    any_enabled = true;

            return any_enabled;
        }

        void GGAKCSVWriter::initStreams()
        {
            for (auto &s : streams)
            {
                if (s.file.is_open())
                    s.file.close();
                s.kind.clear();
                s.header.clear();
                s.path.clear();
                s.day.clear();
                s.part = 1;
                s.bytes = 0;
                s.enabled = false;
            }

            streams[S_FRAMES].kind = "frames";
            streams[S_FRAMES].header = FRAME_HEADER;

            streams[S_MAG].kind = "mag";
            streams[S_MAG].header = MAG_HEADER;

            streams[S_PARTICLE].kind = "particle";
            streams[S_PARTICLE].header = PARTICLE_HEADER;

            streams[S_ESA].kind = "esa";
            std::string esa = "host_utc,elapsed_s,master_counter,direction,record_index,seq";
            constexpr int bins = (SKIF_ESA_DATA_LEN - 1) / 2;
            for (int i = 0; i < bins; i++)
                esa += "," + esa_column_name(i);
            esa += ",checksum_ok";
            streams[S_ESA].header = esa;

            streams[S_SKL].kind = "skl";
            std::string skl = "host_utc,elapsed_s,frame_type,master_counter,record_index,seq,n_values";
            for (int i = 0; i < SKL_MAX_BINS; i++)
                skl += ",bin" + std::to_string(i + 1);
            skl += ",checksum_ok";
            streams[S_SKL].header = skl;

            streams[S_HK].kind = "hk";
            streams[S_HK].header = HK_HEADER;
        }

        bool GGAKCSVWriter::openStream(Stream &s)
        {
            const bool is_new = file_is_empty(s.path);
            if (!is_new && !file_header_matches(s.path, s.header))
            {
                logger->error("Refusing to append to GGAK CSV with a different header: " + s.path);
                return false;
            }

            s.file.open(s.path, std::ios::out | std::ios::app);
            if (!s.file.is_open())
                return false;

            s.file.imbue(std::locale::classic());
            if (is_new)
                s.file << s.header << "\n";
            s.file.flush();

            std::error_code ec;
            const auto size = std::filesystem::file_size(s.path, ec);
            s.bytes = ec ? 0 : size;
            return true;
        }

        std::string GGAKCSVWriter::partPath(const Stream &s, const std::string &day, size_t index) const
        {
            const std::string base = prefix + "_" + s.kind + "_" + day;
            if (index <= 1)
                return (std::filesystem::path(directory) / (base + ".csv")).string();

            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_%04zu.csv", index);
            return (std::filesystem::path(directory) / (base + suffix)).string();
        }

        GGAKCSVWriter::PartInfo GGAKCSVWriter::findLatestPart(const Stream &s) const
        {
            const std::string base = prefix + "_" + s.kind + "_";
            PartInfo latest;
            const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };

            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(directory, ec))
            {
                const std::string name = entry.path().filename().string();
                if (name.rfind(base, 0) != 0)
                    continue;
                if (name.size() < base.size() + 14)
                    continue;
                if (name.substr(name.size() - 4) != ".csv")
                    continue;

                const std::string rest = name.substr(base.size(), name.size() - base.size() - 4);
                if (rest.size() < 10)
                    continue;

                const std::string day = rest.substr(0, 10);
                if (!is_digit(day[0]) || !is_digit(day[1]) || !is_digit(day[2]) || !is_digit(day[3]) ||
                    day[4] != '-' ||
                    !is_digit(day[5]) || !is_digit(day[6]) || day[7] != '-' ||
                    !is_digit(day[8]) || !is_digit(day[9]))
                    continue;

                size_t index = 1;
                if (rest.size() > 10)
                {
                    if (rest[10] != '_')
                        continue;
                    const std::string number = rest.substr(11);
                    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
                        continue;

                    try
                    {
                        index = static_cast<size_t>(std::stoul(number));
                    }
                    catch (...)
                    {
                        continue;
                    }
                    if (index == 0)
                        continue;
                }

                if (day > latest.day || (day == latest.day && index > latest.index))
                    latest = {day, index};
            }

            return latest;
        }

        GGAKCSVWriter::PartInfo GGAKCSVWriter::choosePart(const Stream &s, const std::string &day) const
        {
            const PartInfo latest = findLatestPart(s);
            if (latest.index == 0 || latest.day != day)
                return {day, 1};

            size_t index = latest.index;
            while (true)
            {
                const std::string path = partPath(s, day, index);
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                    return {day, index};

                if (!file_header_matches(path, s.header))
                {
                    index++;
                    continue;
                }

                const auto size = std::filesystem::file_size(path, ec);
                if (!ec && max_bytes > 0 && size >= max_bytes)
                {
                    index++;
                    continue;
                }

                return {day, index};
            }
        }

        bool GGAKCSVWriter::ensureDay(Stream &s, const std::string &day)
        {
            if (!s.enabled)
                return false;

            if (!s.file.is_open())
            {
                const PartInfo part = choosePart(s, day);
                s.day = part.day;
                s.part = part.index;
                s.path = partPath(s, s.day, s.part);

                if (!openStream(s))
                {
                    logger->error("Failed to open GGAK " + s.kind + " CSV: " + s.path);
                    s.enabled = false;
                    return false;
                }

                logger->info("Opened GGAK " + s.kind + " CSV: " + s.path);
            }

            if (s.day != day)
                rotate(s, day, true);
            return s.file.is_open();
        }

        void GGAKCSVWriter::rotate(Stream &s, const std::string &day, bool new_day)
        {
            if (!s.file.is_open())
                return;

            s.file.close();
            if (new_day)
            {
                s.day = day;
                s.part = choosePart(s, day).index;
            }
            else
            {
                s.part++;
            }
            s.path = partPath(s, s.day, s.part);

            if (!openStream(s))
                logger->error("Failed to open next GGAK " + s.kind + " CSV part: " + s.path);
            else
                logger->info("Rotated GGAK " + s.kind + " CSV to " + s.path);
        }

        void GGAKCSVWriter::checkRotate(Stream &s)
        {
            if (max_bytes == 0 || !s.file.is_open())
                return;

            const auto pos = s.file.tellp();
            if (pos != std::streampos(-1))
                s.bytes = static_cast<uint64_t>(static_cast<std::streamoff>(pos));

            if (s.bytes >= max_bytes)
                rotate(s, s.day, false);
        }

        void GGAKCSVWriter::writeFrame(const uint8_t *cadu, const GGAKReader &reader)
        {
            GGAKFrameHeader hdr = parse_header(cadu);
            const bool checksum_ok = validate_checksum(cadu);
            const double elapsed_s = hdr.elapsed_seconds();

            std::string host_utc;
            std::string day_tag;
            now_stamp(host_utc, day_tag);

            if (streams[S_FRAMES].enabled)
            {
                Stream &s = streams[S_FRAMES];
                if (ensureDay(s, day_tag))
                {
                    s.file << csv_escape(host_utc) << ','
                           << std::fixed << std::setprecision(3) << elapsed_s << ','
                           << hex_encode(cadu, ASM_SIZE) << ','
                           << static_cast<int>(hdr.frame_type) << ','
                           << csv_escape(frame_type_name(hdr.frame_type)) << ','
                           << hdr.master_counter << ','
                           << hdr.channel_counter << ','
                           << static_cast<int>(hdr.source_id) << ','
                           << hdr.coarse_time << ','
                           << static_cast<int>(hdr.fine_time) << ','
                           << (checksum_ok ? 1 : 0) << ','
                           << read_be16(cadu + CADU_SIZE - CHECKSUM_SIZE) << ','
                           << hex_encode(cadu + PAYLOAD_OFFSET, PAYLOAD_SIZE) << '\n';
                    checkRotate(s);
                }
            }

            if (streams[S_MAG].enabled)
                writeMag(streams[S_MAG], hdr.master_counter, host_utc, day_tag, elapsed_s, checksum_ok, reader);

            if (streams[S_PARTICLE].enabled)
                writeParticle(streams[S_PARTICLE], hdr.master_counter, host_utc, day_tag, elapsed_s, checksum_ok, reader);

            if (streams[S_ESA].enabled)
            {
                writeEsa(streams[S_ESA], "V", hdr.master_counter, host_utc, day_tag, elapsed_s,
                         checksum_ok, reader.subpackets_20, esa20_written);
                writeEsa(streams[S_ESA], "G", hdr.master_counter, host_utc, day_tag, elapsed_s,
                         checksum_ok, reader.subpackets_30, esa30_written);
            }

            if (streams[S_SKL].enabled)
                writeSkl(streams[S_SKL], hdr.frame_type, hdr.master_counter, host_utc, day_tag, elapsed_s,
                         checksum_ok, reader);

            if (streams[S_HK].enabled)
            {
                writeHk(streams[S_HK], "hk_a", hdr.master_counter, host_utc, day_tag, elapsed_s,
                        checksum_ok, reader.subpackets_00, hk00_written);
                writeHk(streams[S_HK], "hk_ser", hdr.master_counter, host_utc, day_tag, elapsed_s,
                        checksum_ok, reader.subpackets_10, hk10_written);
            }

            for (auto &s : streams)
                if (s.file.is_open())
                    s.file.flush();
        }

        void GGAKCSVWriter::writeMag(Stream &s, uint16_t master_counter, const std::string &host_utc,
                                     const std::string &day, double elapsed_s, bool checksum_ok,
                                     const GGAKReader &reader)
        {
            const size_t start = mag_written;
            if (start >= reader.mag_readings.size())
                return;
            if (!ensureDay(s, day))
            {
                mag_written = reader.mag_readings.size();
                return;
            }

            for (size_t i = start; i < reader.mag_readings.size(); i++)
            {
                const auto &r = reader.mag_readings[i];
                const int rec = static_cast<int>(i - start);

                s.file << csv_escape(host_utc) << ','
                       << std::fixed << std::setprecision(3) << elapsed_s << ','
                       << master_counter << ',' << rec << ','
                       << r.mag_total_nt() << ','
                       << r.mag_x_nt() << ','
                       << r.mag_y_nt() << ','
                       << r.mag_z_nt() << ','
                       << r.voltage() << ','
                       << r.mag_total_raw << ','
                       << r.mag_x_raw << ','
                       << r.mag_y_raw << ','
                       << r.mag_z_raw << ','
                       << static_cast<int>(r.status_flag) << ','
                       << (r.is_valid() ? 1 : 0) << ','
                       << (checksum_ok ? 1 : 0) << '\n';
                checkRotate(s);
            }

            mag_written = reader.mag_readings.size();
        }

        void GGAKCSVWriter::writeParticle(Stream &s, uint16_t master_counter, const std::string &host_utc,
                                          const std::string &day, double elapsed_s, bool checksum_ok,
                                          const GGAKReader &reader)
        {
            const size_t start = particle_written;
            if (start >= reader.particle_readings.size())
                return;
            if (!ensureDay(s, day))
            {
                particle_written = reader.particle_readings.size();
                return;
            }

            for (size_t i = start; i < reader.particle_readings.size(); i++)
            {
                const auto &r = reader.particle_readings[i];
                const int rec = static_cast<int>(i - start);

                s.file << csv_escape(host_utc) << ','
                       << std::fixed << std::setprecision(3) << elapsed_s << ','
                       << master_counter << ',' << rec << ','
                       << static_cast<int>(r.sub_type);

                for (int ch = 0; ch < PARTICLE_CHANNELS; ch++)
                    s.file << ',' << (ch < static_cast<int>(r.channels.size()) ? r.channels[ch] : 0);

                s.file << ',' << (checksum_ok ? 1 : 0) << '\n';
                checkRotate(s);
            }

            particle_written = reader.particle_readings.size();
        }

        void GGAKCSVWriter::writeEsa(Stream &s, const char *direction, uint16_t master_counter,
                                     const std::string &host_utc, const std::string &day, double elapsed_s,
                                     bool checksum_ok, const std::vector<SubPacket> &packets, size_t &written)
        {
            const size_t start = written;
            if (start >= packets.size())
                return;
            if (!ensureDay(s, day))
            {
                written = packets.size();
                return;
            }

            constexpr int bins = (SKIF_ESA_DATA_LEN - 1) / 2;

            for (size_t i = start; i < packets.size(); i++)
            {
                const auto &p = packets[i];
                const int rec = static_cast<int>(i - start);

                s.file << csv_escape(host_utc) << ','
                       << std::fixed << std::setprecision(3) << elapsed_s << ','
                       << master_counter << ','
                       << direction << ',' << rec << ',' << static_cast<int>(p.seq);

                for (int b = 0; b < bins; b++)
                {
                    s.file << ',';
                    if (b < static_cast<int>(p.channels.size()))
                        s.file << p.channels[b];
                }

                s.file << ',' << (checksum_ok ? 1 : 0) << '\n';
                checkRotate(s);
            }

            written = packets.size();
        }

        void GGAKCSVWriter::writeSkl(Stream &s, uint8_t frame_type, uint16_t master_counter,
                                     const std::string &host_utc, const std::string &day, double elapsed_s,
                                     bool checksum_ok, const GGAKReader &reader)
        {
            const size_t start = skl_written;
            if (start >= reader.subpackets_5c.size())
                return;
            if (!ensureDay(s, day))
            {
                skl_written = reader.subpackets_5c.size();
                return;
            }

            for (size_t i = start; i < reader.subpackets_5c.size(); i++)
            {
                const auto &p = reader.subpackets_5c[i];
                const int rec = static_cast<int>(i - start);

                s.file << csv_escape(host_utc) << ','
                       << std::fixed << std::setprecision(3) << elapsed_s << ','
                       << static_cast<int>(frame_type) << ','
                       << master_counter << ',' << rec << ',' << static_cast<int>(p.seq) << ','
                       << p.channels.size();

                for (int b = 0; b < SKL_MAX_BINS; b++)
                {
                    s.file << ',';
                    if (b < static_cast<int>(p.channels.size()))
                        s.file << p.channels[b];
                }

                s.file << ',' << (checksum_ok ? 1 : 0) << '\n';
                checkRotate(s);
            }

            skl_written = reader.subpackets_5c.size();
        }

        void GGAKCSVWriter::writeHk(Stream &s, const char *record_type, uint16_t master_counter,
                                    const std::string &host_utc, const std::string &day, double elapsed_s,
                                    bool checksum_ok, const std::vector<SubPacket> &packets, size_t &written)
        {
            const size_t start = written;
            if (start >= packets.size())
                return;
            if (!ensureDay(s, day))
            {
                written = packets.size();
                return;
            }

            for (size_t i = start; i < packets.size(); i++)
            {
                const auto &p = packets[i];
                const int rec = static_cast<int>(i - start);

                s.file << csv_escape(host_utc) << ','
                       << std::fixed << std::setprecision(3) << elapsed_s << ','
                       << master_counter << ',' << record_type << ',' << rec << ','
                       << static_cast<int>(p.seq);

                for (int ch = 0; ch < 5; ch++)
                {
                    s.file << ',';
                    if (ch < static_cast<int>(p.channels.size()))
                        s.file << p.channels[ch];
                }

                s.file << ',' << (checksum_ok ? 1 : 0) << '\n';
                checkRotate(s);
            }

            written = packets.size();
        }

        bool GGAKCSVWriter::isOpen() const
        {
            for (const auto &s : streams)
                if (s.file.is_open())
                    return true;
            return false;
        }

        std::vector<std::string> GGAKCSVWriter::getPaths() const
        {
            std::vector<std::string> paths;
            for (const auto &s : streams)
                if (!s.path.empty())
                    paths.push_back(s.path);
            return paths;
        }

        void GGAKCSVWriter::close()
        {
            for (auto &s : streams)
                if (s.file.is_open())
                    s.file.close();
        }
    } // namespace ggak
} // namespace elektro_arktika
