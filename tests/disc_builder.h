#pragma once

// Small made-up XBE files and disc images: the tests need no game file.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/byteorder.h"

constexpr size_t kTestSector = 2048;

struct DiscNode {
    std::string name;
    std::vector<uint8_t> data;
    std::vector<DiscNode> children;
    bool directory = false;
};

// Appends a directory table and what it holds; the entries form a binary tree, each sector zero or 0xFF as on a disc.
inline void place_directory(const std::vector<DiscNode>& nodes, std::vector<uint8_t>& image, uint32_t& sector, uint32_t& size) {
    std::vector<uint32_t> offsets;
    uint32_t table_size = 0;
    for (const DiscNode& node : nodes) {
        offsets.push_back(table_size);
        table_size += static_cast<uint32_t>((14 + node.name.size() + 3) & ~size_t{3});
    }
    size = (table_size + kTestSector - 1) & ~static_cast<uint32_t>(kTestSector - 1);
    if (size == 0) size = kTestSector;
    sector = static_cast<uint32_t>(image.size() / kTestSector);
    image.resize(image.size() + size, 0xFF);
    for (size_t i = 0; i < nodes.size(); ++i) {
        uint32_t child_sector = 0, child_size = 0;
        if (nodes[i].directory) {
            place_directory(nodes[i].children, image, child_sector, child_size);
        } else {
            child_sector = static_cast<uint32_t>(image.size() / kTestSector);
            child_size = static_cast<uint32_t>(nodes[i].data.size());
            image.insert(image.end(), nodes[i].data.begin(), nodes[i].data.end());
            image.resize((image.size() + kTestSector - 1) & ~(kTestSector - 1), 0);
        }
        uint8_t* raw = image.data() + sector * kTestSector + offsets[i];
        const uint32_t left = 2 * i + 1 < nodes.size() ? offsets[2 * i + 1] / 4 : 0;
        const uint32_t right = 2 * i + 2 < nodes.size() ? offsets[2 * i + 2] / 4 : 0;
        raw[0] = static_cast<uint8_t>(left), raw[1] = static_cast<uint8_t>(left >> 8);
        raw[2] = static_cast<uint8_t>(right), raw[3] = static_cast<uint8_t>(right >> 8);
        mc3::store_le32(raw + 4, child_sector);
        mc3::store_le32(raw + 8, child_size);
        raw[12] = nodes[i].directory ? 0x10 : 0x20;
        raw[13] = static_cast<uint8_t>(nodes[i].name.size());
        std::memcpy(raw + 14, nodes[i].name.data(), nodes[i].name.size());
    }
}

inline std::vector<uint8_t> make_xiso(const std::vector<DiscNode>& root) {
    std::vector<uint8_t> image(33 * kTestSector, 0);
    uint32_t sector = 0, size = 0;
    place_directory(root, image, sector, size);
    uint8_t* descriptor = image.data() + 32 * kTestSector;
    std::memcpy(descriptor, "MICROSOFT*XBOX*MEDIA", 20);
    mc3::store_le32(descriptor + 20, sector);
    mc3::store_le32(descriptor + 24, size);
    return image;
}

inline void put_le32(std::vector<uint8_t>& data, size_t offset, uint32_t value) { mc3::store_le32(data.data() + offset, value); }

constexpr uint32_t kTestBase = 0x10000;
constexpr uint32_t kTestEntry = 0x11010, kTestThunk = 0x11020, kTestTitleId = 0x54540079;

// base 0x10000; header 0x1000 bytes; one section of 0x100 bytes at 0x11000, in the file at 0x1000.
inline std::vector<uint8_t> make_xbe(const char* title = "Test Game", bool retail = true) {
    std::vector<uint8_t> xbe(0x1100, 0);
    std::memcpy(xbe.data(), "XBEH", 4);
    put_le32(xbe, 0x104, kTestBase);
    put_le32(xbe, 0x108, 0x1000);    // header size
    put_le32(xbe, 0x10C, 0x2000);    // image size
    put_le32(xbe, 0x118, kTestBase + 0x200);  // certificate
    put_le32(xbe, 0x11C, 1);         // one section
    put_le32(xbe, 0x120, kTestBase + 0x300);
    put_le32(xbe, 0x12C, 0x11040);   // TLS directory, in the section
    put_le32(xbe, 0x130, 0x40000);   // stack commit
    put_le32(xbe, 0x128, kTestEntry ^ (retail ? 0xA8FC57ABu : 0x94859D4Bu));
    put_le32(xbe, 0x158, kTestThunk ^ (retail ? 0x5B6D40B6u : 0xEFB1F152u));
    put_le32(xbe, 0x160, 1);         // one library
    put_le32(xbe, 0x164, kTestBase + 0x500);
    put_le32(xbe, 0x200 + 8, kTestTitleId);
    for (size_t i = 0; title[i]; ++i) xbe[0x200 + 0x0C + i * 2] = static_cast<uint8_t>(title[i]);
    put_le32(xbe, 0x300, 0x4 | 0x2);  // executable, preload
    put_le32(xbe, 0x304, 0x11000);
    put_le32(xbe, 0x308, 0x100);
    put_le32(xbe, 0x30C, 0x1000);
    put_le32(xbe, 0x310, 0x100);
    put_le32(xbe, 0x314, kTestBase + 0x400);
    std::memcpy(xbe.data() + 0x400, ".text", 5);
    std::memcpy(xbe.data() + 0x500, "XAPILIB", 7);
    xbe[0x508] = 1, xbe[0x50C] = 0xE9, xbe[0x50D] = 0x16;  // version 1.0.5865
    put_le32(xbe, 0x1040, 0x11050);  // TLS raw data start
    put_le32(xbe, 0x1044, 0x11060);  // and end
    put_le32(xbe, 0x1050, 0xCAFE);
    return xbe;
}
