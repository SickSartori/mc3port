// Xbox kernel crypto (Xc*): SHA-1, HMAC-SHA1 and RC4. XAPI signs saves with HMAC (XCalculateSignature) and the
// network libraries hash their keys at startup.
//
// DES/3DES only protect System Link and Xbox Live traffic, which the port does not carry: the network stack still
// derives its keys at startup, so the block cipher is a pass-through (logged once). Key parity is computed for real.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "pc/kernel_internal.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

#pragma pack(push, 1)
// Fits in the caller's A_SHA_CTX (116 bytes on Xbox).
struct ShaContext {
    uint32_t state[5];
    uint64_t length;  // bytes hashed so far
    uint8_t block[64];
    uint32_t block_used;
};
#pragma pack(pop)
static_assert(sizeof(ShaContext) <= 116);

uint32_t rotate_left(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

void sha_transform(ShaContext& context, const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = context.state[0], b = context.state[1], c = context.state[2], d = context.state[3], e = context.state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t temp = rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temp;
    }
    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
    context.state[4] += e;
}

void __stdcall XcSHAInit(ShaContext* context) {
    std::memset(context, 0, sizeof(*context));
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
}

void __stdcall XcSHAUpdate(ShaContext* context, const uint8_t* input, uint32_t length) {
    context->length += length;
    while (length) {
        const uint32_t take = std::min<uint32_t>(length, 64 - context->block_used);
        std::memcpy(context->block + context->block_used, input, take);
        context->block_used += take;
        input += take;
        length -= take;
        if (context->block_used == 64) {
            sha_transform(*context, context->block);
            context->block_used = 0;
        }
    }
}

void __stdcall XcSHAFinal(ShaContext* context, uint8_t* digest) {
    const uint64_t bits = context->length * 8;
    const uint8_t pad = 0x80;
    XcSHAUpdate(context, &pad, 1);
    const uint8_t zero = 0;
    while (context->block_used != 56) XcSHAUpdate(context, &zero, 1);
    uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) length_bytes[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    XcSHAUpdate(context, length_bytes, 8);
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>(context->state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(context->state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(context->state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(context->state[i]);
    }
}

// HMAC-SHA1 of data || data2.
void __stdcall XcHMAC(const uint8_t* key, uint32_t key_length, const uint8_t* data, uint32_t data_length,
                      const uint8_t* data2, uint32_t data2_length, uint8_t* digest) {
    uint8_t key_block[64] = {};
    if (key_length > 64) {
        ShaContext context;
        XcSHAInit(&context);
        XcSHAUpdate(&context, key, key_length);
        XcSHAFinal(&context, key_block);
    } else if (key_length) {
        std::memcpy(key_block, key, key_length);
    }
    uint8_t pad[64];
    ShaContext context;
    for (int i = 0; i < 64; ++i) pad[i] = key_block[i] ^ 0x36;
    XcSHAInit(&context);
    XcSHAUpdate(&context, pad, 64);
    if (data && data_length) XcSHAUpdate(&context, data, data_length);
    if (data2 && data2_length) XcSHAUpdate(&context, data2, data2_length);
    uint8_t inner[20];
    XcSHAFinal(&context, inner);
    for (int i = 0; i < 64; ++i) pad[i] = key_block[i] ^ 0x5C;
    XcSHAInit(&context);
    XcSHAUpdate(&context, pad, 64);
    XcSHAUpdate(&context, inner, 20);
    XcSHAFinal(&context, digest);
}

// RC4 state: 256-byte permutation followed by the two indices.
void __stdcall XcRC4Key(uint8_t* state, uint32_t key_length, const uint8_t* key) {
    for (int i = 0; i < 256; ++i) state[i] = static_cast<uint8_t>(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; ++i) {
        j = static_cast<uint8_t>(j + state[i] + key[i % key_length]);
        std::swap(state[i], state[j]);
    }
    state[256] = 0;
    state[257] = 0;
}

void __stdcall XcRC4Crypt(uint8_t* state, uint32_t length, uint8_t* data) {
    uint8_t i = state[256];
    uint8_t j = state[257];
    for (uint32_t n = 0; n < length; ++n) {
        i = static_cast<uint8_t>(i + 1);
        j = static_cast<uint8_t>(j + state[i]);
        std::swap(state[i], state[j]);
        data[n] ^= state[static_cast<uint8_t>(state[i] + state[j])];
    }
    state[256] = i;
    state[257] = j;
}

// Sets the odd-parity bit (bit 0) of every key byte, as DES expects.
void __stdcall XcDESKeyParity(uint8_t* key, uint32_t length) {
    for (uint32_t i = 0; i < length; ++i) {
        uint8_t bits = key[i] >> 1;
        bits ^= bits >> 4;
        bits ^= bits >> 2;
        bits ^= bits >> 1;
        key[i] = static_cast<uint8_t>((key[i] & 0xFE) | ((bits & 1) ^ 1));
    }
}

void warn_no_network_cipher() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        log_line("crypto: DES block cipher is a pass-through (network encryption is not supported)");
    }
}

void __stdcall XcKeyTable(uint32_t /*cipher*/, uint8_t* key_table, const uint8_t* key) {
    warn_no_network_cipher();
    std::memcpy(key_table, key, 8);
}

void __stdcall XcBlockCrypt(uint32_t /*cipher*/, uint8_t* output, const uint8_t* input, const uint8_t*, uint32_t) {
    warn_no_network_cipher();
    std::memmove(output, input, 8);
}

void __stdcall XcBlockCryptCBC(uint32_t /*cipher*/, uint32_t length, uint8_t* output, const uint8_t* input,
                               const uint8_t*, uint32_t, uint8_t*) {
    warn_no_network_cipher();
    std::memmove(output, input, length);
}

// Known-answer test: RFC 2202-style HMAC-SHA1 of the "quick brown fox" sentence with key "key".
bool crypto_self_test() {
    constexpr char kKey[] = "key";
    constexpr char kMessage[] = "The quick brown fox jumps over the lazy dog";
    constexpr uint8_t kExpected[20] = {0xDE, 0x7C, 0x9B, 0x85, 0xB8, 0xB7, 0x8A, 0xA6, 0xBC, 0x8A,
                                       0x7A, 0x36, 0xF7, 0x0A, 0x90, 0x70, 0x1C, 0x9D, 0xB4, 0xD9};
    uint8_t digest[20];
    XcHMAC(reinterpret_cast<const uint8_t*>(kKey), 3, reinterpret_cast<const uint8_t*>(kMessage),
           sizeof(kMessage) - 1, nullptr, 0, digest);
    return std::memcmp(digest, kExpected, sizeof(digest)) == 0;
}

}  // namespace

void register_crypto_functions() {
    if (!crypto_self_test()) log_line("crypto: HMAC-SHA1 self-test FAILED, saves would get wrong signatures");
    register_kernel(335, &XcSHAInit);
    register_kernel(336, &XcSHAUpdate);
    register_kernel(337, &XcSHAFinal);
    register_kernel(340, &XcHMAC);
    register_kernel(338, &XcRC4Key);
    register_kernel(339, &XcRC4Crypt);
    register_kernel(346, &XcDESKeyParity);
    register_kernel(347, &XcKeyTable);
    register_kernel(348, &XcBlockCrypt);
    register_kernel(349, &XcBlockCryptCBC);
}

}  // namespace mc3::pc
