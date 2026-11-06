// The XBE parser and the disc images: XISO, full dump, split, CCI, and damaged ones.

#include <algorithm>

#include "core/xbe.h"
#include "core/xdvdfs.h"
#include "disc_builder.h"
#include "test.h"

using namespace mc3;

namespace {

void test_xbe() {
    std::string error;
    auto xbe = Xbe::parse(make_xbe(), &error);
    CHECK(xbe.has_value());
    if (!xbe) return;
    CHECK(xbe->is_retail());
    CHECK(xbe->base_address() == kTestBase);
    CHECK(xbe->entry_point() == kTestEntry);
    CHECK(xbe->kernel_thunk_address() == kTestThunk);
    CHECK(xbe->title() == "Test Game");
    CHECK(xbe->title_id() == kTestTitleId);
    CHECK(xbe->stack_commit() == 0x40000);
    CHECK(xbe->sections().size() == 1 && xbe->sections()[0].name == ".text" && xbe->sections()[0].executable());
    CHECK(xbe->libraries().size() == 1 && xbe->libraries()[0].name == "XAPILIB" && xbe->libraries()[0].build == 5865);
    CHECK(xbe->file_offset(kTestBase + 0x10) == 0x10u);
    CHECK(xbe->file_offset(0x11010) == 0x1010u);
    CHECK(!xbe->file_offset(0x11100).has_value());      // past the section
    CHECK(!xbe->file_offset(0x110FF, 2).has_value());   // crosses its end
    CHECK(xbe->tls().has_value() && xbe->tls()->raw_data_end == 0x11060);

    auto debug = Xbe::parse(make_xbe("Debug", false), &error);
    CHECK(debug && !debug->is_retail() && debug->entry_point() == kTestEntry && debug->kernel_thunk_address() == kTestThunk);

    const auto refused = [&](std::vector<uint8_t> file) { return !Xbe::parse(std::move(file), &error).has_value(); };
    std::vector<uint8_t> bad = make_xbe();
    bad[0] = 'Y';
    CHECK(refused(bad));
    CHECK(refused(std::vector<uint8_t>(0x20, 0)));
    bad = make_xbe();
    put_le32(bad, 0x108, 0x100000);  // header larger than the file
    CHECK(refused(bad));
    bad = make_xbe();
    put_le32(bad, 0x11C, 0x1000000);  // section headers out of the header
    CHECK(refused(bad));
    bad = make_xbe();
    put_le32(bad, 0x310, 0x100000);  // a section past the end of the file
    CHECK(refused(bad));
    bad = make_xbe();
    put_le32(bad, 0x128, 0);  // entry point nowhere
    CHECK(refused(bad));
}

std::vector<uint8_t> sample_data() {
    std::vector<uint8_t> data(5000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);
    return data;
}

std::vector<DiscNode> sample_tree() {
    return {{"default.xbe", {1, 2, 3}},
            {"big.bin", sample_data()},
            {"Flash", {}, {{"menu.xbck", {9, 8}}, {"Deep", {}, {{"a.txt", {7}}}, true}}, true},
            {"empty.dat", {}}};
}

void check_sample(const XdvdfsImage& image) {
    CHECK(image.list(image.root()).size() == 4);
    CHECK(image.find("").has_value() && image.find("")->is_directory);
    CHECK(image.find("DEFAULT.XBE").has_value());  // as the Xbox: any case
    CHECK(image.find("flash\\MENU.xbck").has_value());
    CHECK(image.find("Flash/Deep/a.txt").has_value());
    CHECK(!image.find("nothing").has_value());
    CHECK(!image.find("default.xbe/more").has_value());
    const auto big = image.find("big.bin");
    CHECK(big.has_value());
    if (!big) return;
    CHECK(image.read_all(*big) == sample_data());
    uint8_t part[10] = {};
    CHECK(image.read(*big, 4995, part, 10) == 5);  // short at the end
    CHECK(part[0] == static_cast<uint8_t>(4995 * 7) && part[4] == static_cast<uint8_t>(4999 * 7));
    CHECK(image.read(*big, 5000, part, 10) == 0);
    CHECK(image.read(image.root(), 0, part, 10) == 0);  // not a file
    const auto empty = image.find("empty.dat");
    CHECK(empty && image.read_all(*empty).empty());
}

void test_images(const std::filesystem::path& folder) {
    std::string error;
    const std::vector<uint8_t> iso = make_xiso(sample_tree());

    write_file(folder / "plain.iso", iso);
    auto plain = XdvdfsImage::open(folder / "plain.iso", &error);
    CHECK(plain != nullptr);
    if (plain) check_sample(*plain);

    // A full disc dump: the game partition further in.
    constexpr uint64_t kOffset = 0x2080000;
    std::vector<uint8_t> dump(kOffset, 0);
    dump.insert(dump.end(), iso.begin(), iso.end());
    write_file(folder / "dump.iso", dump);
    auto full = XdvdfsImage::open(folder / "dump.iso", &error);
    CHECK(full != nullptr);
    if (full) check_sample(*full);

    // Split in two, in the middle of a file; opened from either part.
    const size_t cut = 33 * kTestSector + 3000;
    write_file(folder / "split.1.iso", iso.data(), cut);
    write_file(folder / "split.2.iso", iso.data() + cut, iso.size() - cut);
    for (const char* name : {"split.1.iso", "split.2.iso"}) {
        auto split = XdvdfsImage::open(folder / name, &error);
        CHECK(split != nullptr);
        if (split) check_sample(*split);
    }
}

// CCI: uniform sectors as LZ4 with padding, the others stored as they are.
std::vector<uint8_t> make_cci(const std::vector<uint8_t>& iso) {
    const size_t sectors = iso.size() / kTestSector;
    std::vector<uint8_t> file(32, 0);
    std::vector<uint32_t> index;
    const auto align = [&] { while (file.size() % 4) file.push_back(0); };
    for (size_t s = 0; s < sectors; ++s) {
        const uint8_t* sector = iso.data() + s * kTestSector;
        const bool uniform = std::all_of(sector, sector + kTestSector, [&](uint8_t b) { return b == sector[0]; });
        align();
        const auto position = static_cast<uint32_t>(file.size() >> 2);
        if (uniform) {
            index.push_back(position | 0x80000000u);
            // One literal, then a copy at distance 1: length 2047 = 4 + 15 + 7 * 255 + 243.
            const uint8_t padding = 3;
            std::vector<uint8_t> block = {padding, 0x1F, sector[0], 1, 0};
            block.insert(block.end(), 7, 255);
            block.push_back(243);
            block.insert(block.end(), padding, 0);
            file.insert(file.end(), block.begin(), block.end());
        } else {
            index.push_back(position);
            file.insert(file.end(), sector, sector + kTestSector);
        }
    }
    align();
    index.push_back(static_cast<uint32_t>(file.size() >> 2));
    const uint64_t index_offset = file.size();
    for (uint32_t entry : index) {
        file.resize(file.size() + 4);
        mc3::store_le32(file.data() + file.size() - 4, entry);
    }
    std::memcpy(file.data(), "CCIM", 4);
    put_le32(file, 4, 32);
    put_le32(file, 8, static_cast<uint32_t>(iso.size()));
    put_le32(file, 16, static_cast<uint32_t>(index_offset));
    put_le32(file, 24, 2048);
    file[28] = 1, file[29] = 2;
    return file;
}

void test_cci(const std::filesystem::path& folder) {
    std::string error;
    const std::vector<uint8_t> iso = make_xiso(sample_tree());
    const std::vector<uint8_t> cci = make_cci(iso);
    CHECK(cci.size() < iso.size());
    write_file(folder / "game.cci", cci);
    auto image = XdvdfsImage::open(folder / "game.cci", &error);
    CHECK(image != nullptr);
    if (image) check_sample(*image);

    std::vector<uint8_t> bad = cci;
    bad[29] = 3;  // another index alignment
    write_file(folder / "bad.cci", bad);
    CHECK(XdvdfsImage::open(folder / "bad.cci", &error) == nullptr);
    bad = cci;
    bad.resize(bad.size() - 8);  // index cut short
    write_file(folder / "short.cci", bad);
    CHECK(XdvdfsImage::open(folder / "short.cci", &error) == nullptr);
    CHECK(error.find("cut short") != std::string::npos);
}

void test_damaged(const std::filesystem::path& folder) {
    std::string error;
    CHECK(XdvdfsImage::open(folder / "missing.iso", &error) == nullptr);
    CHECK(!error.empty());
    write_file(folder / "text.iso", std::string(100000, 'x'));
    CHECK(XdvdfsImage::open(folder / "text.iso", &error) == nullptr);
    write_file(folder / "tiny.iso", std::string(3, 'x'));
    CHECK(XdvdfsImage::open(folder / "tiny.iso", &error) == nullptr);

    // A directory whose tree points back at itself ends, and an entry past the table is left out.
    std::vector<uint8_t> iso = make_xiso({{"a.bin", {1}}, {"b.bin", {2}}});
    const size_t root = 33 * kTestSector;
    const uint32_t second = (14 + 5 + 3) / 4;
    iso[root + second * 4 + 2] = static_cast<uint8_t>(second);  // b's right child is b
    iso[root + second * 4 + 0] = 0xFF, iso[root + second * 4 + 1] = 0x7F;  // and its left is far outside
    write_file(folder / "loop.iso", iso);
    auto image = XdvdfsImage::open(folder / "loop.iso", &error);
    CHECK(image != nullptr);
    if (image) CHECK(image->list(image->root()).size() == 2);

    // A file said to lie past the end of the image reads as nothing, not garbage.
    iso = make_xiso({{"a.bin", std::vector<uint8_t>(100, 1)}});
    mc3::store_le32(iso.data() + root + 4, 0x100000);
    write_file(folder / "past.iso", iso);
    image = XdvdfsImage::open(folder / "past.iso", &error);
    CHECK(image != nullptr);
    if (image) {
        const auto file = image->find("a.bin");
        CHECK(file && image->read_all(*file).empty());
    }
}

}  // namespace

int main() {
    TemporaryFolder folder("mc3port-test-core");
    test_xbe();
    test_images(folder.path);
    test_cci(folder.path);
    test_damaged(folder.path);
    return test_result();
}
