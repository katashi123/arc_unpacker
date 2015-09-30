#include "fmt/kirikiri/xp3_archive_decoder.h"
#include "err.h"
#include "fmt/kirikiri/tlg_image_decoder.h"
#include "fmt/kirikiri/xp3_filter_registry.h"
#include "io/buffered_io.h"
#include "util/encoding.h"
#include "util/pack/zlib.h"

using namespace au;
using namespace au::fmt::kirikiri;

namespace
{
    struct InfoChunk final
    {
        u32 flags;
        u64 file_size_original;
        u64 file_size_compressed;
        std::string name;
    };
}

static const bstr xp3_magic = "XP3\r\n\x20\x0A\x1A\x8B\x67\x01"_b;
static const bstr file_magic = "File"_b;
static const bstr adlr_magic = "adlr"_b;
static const bstr info_magic = "info"_b;
static const bstr segm_magic = "segm"_b;

static int detect_version(io::IO &arc_io)
{
    int version = 1;
    size_t old_pos = arc_io.tell();
    arc_io.seek(19);
    if (arc_io.read_u32_le() == 1)
        version = 2;
    arc_io.seek(old_pos);
    return version;
}

static u64 get_table_offset(io::IO &arc_io, int version)
{
    if (version == 1)
        return arc_io.read_u64_le();

    u64 additional_header_offset = arc_io.read_u64_le();
    u32 minor_version = arc_io.read_u32_le();
    if (minor_version != 1)
        throw err::CorruptDataError("Unexpected XP3 version");

    arc_io.seek(additional_header_offset);
    arc_io.skip(1); // flags?
    arc_io.skip(8); // table size
    return arc_io.read_u64_le();
}

static std::unique_ptr<io::IO> read_raw_table(io::IO &arc_io)
{
    bool use_zlib = arc_io.read_u8() != 0;
    const u64 size_compressed = arc_io.read_u64_le();
    const u64 size_original = use_zlib
        ? arc_io.read_u64_le()
        : size_compressed;

    bstr data = arc_io.read(size_compressed);
    if (use_zlib)
        data = util::pack::zlib_inflate(data);
    return std::unique_ptr<io::IO>(new io::BufferedIO(data));
}

static InfoChunk read_info_chunk(io::IO &table_io)
{
    if (table_io.read(info_magic.size()) != info_magic)
        throw err::CorruptDataError("Expected INFO chunk");
    u64 info_chunk_size = table_io.read_u64_le();

    InfoChunk info_chunk;
    info_chunk.flags = table_io.read_u32_le();
    info_chunk.file_size_original = table_io.read_u64_le();
    info_chunk.file_size_compressed = table_io.read_u64_le();

    size_t file_name_size = table_io.read_u16_le();
    auto name = table_io.read(file_name_size * 2);
    info_chunk.name = util::convert_encoding(name, "utf-16le", "utf-8").str();
    return info_chunk;
}

static bstr read_data_from_segm_chunk(io::IO &table_io, io::IO &arc_io)
{
    if (table_io.read(segm_magic.size()) != segm_magic)
        throw err::CorruptDataError("Expected SEGM chunk");

    u64 segm_chunk_size = table_io.read_u64_le();
    if (segm_chunk_size % 28 != 0)
        throw err::CorruptDataError("Unexpected SEGM chunk size");
    bstr full_data;
    size_t initial_pos = table_io.tell();
    while (segm_chunk_size > table_io.tell() - initial_pos)
    {
        u32 segm_flags = table_io.read_u32_le();
        u64 data_offset = table_io.read_u64_le();
        u64 data_size_original = table_io.read_u64_le();
        u64 data_size_compressed = table_io.read_u64_le();
        arc_io.seek(data_offset);

        bool use_zlib = (segm_flags & 7) > 0;
        if (use_zlib)
        {
            auto data_compressed = arc_io.read(data_size_compressed);
            auto data_uncompressed = util::pack::zlib_inflate(data_compressed);
            full_data += data_uncompressed;
        }
        else
        {
            full_data += arc_io.read(data_size_original);
        }
    }

    return full_data;
}

static u32 read_key_from_adlr_chunk(io::IO &table_io)
{
    if (table_io.read(adlr_magic.size()) != adlr_magic)
        throw err::CorruptDataError("Expected ADLR chunk");

    u64 adlr_chunk_size = table_io.read_u64_le();
    if (adlr_chunk_size != 4)
        throw err::CorruptDataError("Unexpected ADLR chunk size");

    return table_io.read_u32_le();
}

static std::unique_ptr<File> read_file(
    io::IO &arc_io, io::IO &table_io, const Xp3FilterFunc &filter_func)
{
    std::unique_ptr<File> target_file(new File());

    if (table_io.read(file_magic.size()) != file_magic)
        throw err::CorruptDataError("Expected FILE chunk");

    u64 file_chunk_size = table_io.read_u64_le();
    size_t file_chunk_start_offset = table_io.tell();

    auto info_chunk = read_info_chunk(table_io);
    auto data = read_data_from_segm_chunk(table_io, arc_io);
    auto key = read_key_from_adlr_chunk(table_io);

    if (table_io.tell() - file_chunk_start_offset != file_chunk_size)
        throw err::CorruptDataError("Unexpected file data size");

    if (filter_func)
        filter_func(data, key);

    target_file->name = info_chunk.name;
    target_file->io.write(data);
    return target_file;
}

struct Xp3ArchiveDecoder::Priv final
{
    Xp3FilterRegistry filter_registry;
    TlgImageDecoder tlg_image_decoder;
};

Xp3ArchiveDecoder::Xp3ArchiveDecoder() : p(new Priv)
{
    add_decoder(&p->tlg_image_decoder);
}

Xp3ArchiveDecoder::~Xp3ArchiveDecoder()
{
}

void Xp3ArchiveDecoder::register_cli_options(ArgParser &arg_parser) const
{
    p->filter_registry.register_cli_options(arg_parser);
    ArchiveDecoder::register_cli_options(arg_parser);
}

void Xp3ArchiveDecoder::parse_cli_options(const ArgParser &arg_parser)
{
    p->filter_registry.parse_cli_options(arg_parser);
    ArchiveDecoder::parse_cli_options(arg_parser);
}

bool Xp3ArchiveDecoder::is_recognized_internal(File &arc_file) const
{
    return arc_file.io.read(xp3_magic.size()) == xp3_magic;
}

void Xp3ArchiveDecoder::unpack_internal(File &arc_file, FileSaver &saver) const
{
    arc_file.io.skip(xp3_magic.size());

    int version = detect_version(arc_file.io);
    u64 table_offset = get_table_offset(arc_file.io, version);
    arc_file.io.seek(table_offset);
    auto table_io = read_raw_table(arc_file.io);

    Xp3Filter filter(arc_file.name);
    p->filter_registry.set_decoder(filter);

    while (table_io->tell() < table_io->size())
        saver.save(read_file(arc_file.io, *table_io, filter.decoder));
}

static auto dummy = fmt::Registry::add<Xp3ArchiveDecoder>("krkr/xp3");