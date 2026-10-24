#include "pc/mmio.h"

#include "core/byteorder.h"
#include "pc/log.h"

namespace mc3::pc {
namespace {

constexpr uint32_t kCarry = 0x001;
constexpr uint32_t kParity = 0x004;
constexpr uint32_t kAdjust = 0x010;
constexpr uint32_t kZero = 0x040;
constexpr uint32_t kSign = 0x080;
constexpr uint32_t kOverflow = 0x800;
constexpr uint32_t kArithmeticFlags = kCarry | kParity | kAdjust | kZero | kSign | kOverflow;

uint32_t mask_of(uint32_t size) { return size == 4 ? 0xFFFFFFFF : (1u << (size * 8)) - 1; }
uint32_t sign_bit(uint32_t size) { return 1u << (size * 8 - 1); }

DWORD* register_slot(CONTEXT* context, uint32_t index) {
    switch (index & 7) {
        case 0: return &context->Eax;
        case 1: return &context->Ecx;
        case 2: return &context->Edx;
        case 3: return &context->Ebx;
        case 4: return &context->Esp;
        case 5: return &context->Ebp;
        case 6: return &context->Esi;
        default: return &context->Edi;
    }
}

uint32_t get_register(CONTEXT* context, uint32_t index, uint32_t size) {
    if (size == 1) {
        const DWORD value = *register_slot(context, index & 3);
        return index & 4 ? (value >> 8) & 0xFF : value & 0xFF;  // ah/ch/dh/bh for 4..7
    }
    return *register_slot(context, index) & mask_of(size);
}

void set_register(CONTEXT* context, uint32_t index, uint32_t size, uint32_t value) {
    if (size == 1) {
        DWORD* slot = register_slot(context, index & 3);
        if (index & 4) {
            *slot = (*slot & ~0xFF00u) | ((value & 0xFF) << 8);
        } else {
            *slot = (*slot & ~0xFFu) | (value & 0xFF);
        }
    } else if (size == 2) {
        DWORD* slot = register_slot(context, index);
        *slot = (*slot & ~0xFFFFu) | (value & 0xFFFF);
    } else {
        *register_slot(context, index) = value;
    }
}

bool even_parity(uint32_t value) {
    value &= 0xFF;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return (value & 1) == 0;
}

void set_result_flags(CONTEXT* context, uint32_t result, uint32_t size, uint32_t extra) {
    uint32_t flags = context->EFlags & ~kArithmeticFlags;
    result &= mask_of(size);
    if (result == 0) flags |= kZero;
    if (result & sign_bit(size)) flags |= kSign;
    if (even_parity(result)) flags |= kParity;
    context->EFlags = flags | extra;
}

void logic_flags(CONTEXT* context, uint32_t result, uint32_t size) { set_result_flags(context, result, size, 0); }

void compare_flags(CONTEXT* context, uint32_t a, uint32_t b, uint32_t size) {
    const uint32_t mask = mask_of(size);
    a &= mask;
    b &= mask;
    const uint32_t result = (a - b) & mask;
    uint32_t extra = 0;
    if (a < b) extra |= kCarry;
    if ((a ^ b) & (a ^ result) & sign_bit(size)) extra |= kOverflow;
    if ((a ^ b ^ result) & 0x10) extra |= kAdjust;
    set_result_flags(context, result, size, extra);
}

// Length of a ModRM memory operand (ModRM byte, SIB, displacement); 0 for a register operand.
uint32_t modrm_length(const uint8_t* modrm) {
    const uint8_t mod = modrm[0] >> 6;
    const uint8_t rm = modrm[0] & 7;
    if (mod == 3) return 0;
    uint32_t length = 1;
    if (rm == 4) {
        const uint8_t base = modrm[1] & 7;
        ++length;
        if (mod == 0 && base == 5) length += 4;
    } else if (mod == 0 && rm == 5) {
        length += 4;
    }
    if (mod == 1) length += 1;
    if (mod == 2) length += 4;
    return length;
}

uint32_t immediate(const uint8_t* p, uint32_t size) {
    return size == 1 ? p[0] : size == 2 ? load_le16(p) : load_le32(p);
}

}  // namespace

bool emulate_mmio_access(CONTEXT* context, uint32_t address, const MmioDevice& device) {
    const auto* code = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(context->Eip));
    uint32_t prefix = 0;
    uint32_t operand = 4;
    while (code[prefix] == 0x66 || code[prefix] == 0x3E || code[prefix] == 0x2E) {
        if (code[prefix] == 0x66) operand = 2;
        ++prefix;
    }
    const uint8_t* p = code + prefix;
    const uint8_t opcode = p[0];
    const uint8_t reg = (p[1] >> 3) & 7;
    uint32_t length = 0;

    auto rmw = [&](uint32_t size, uint32_t source, bool is_and) {
        const uint32_t result = is_and ? device.read(address, size) & source : device.read(address, size) | source;
        device.write(address, size, result & mask_of(size));
        logic_flags(context, result, size);
    };

    switch (opcode) {
        case 0x8B:  // mov r, m
        case 0x8A: {
            const uint32_t size = opcode == 0x8A ? 1 : operand;
            length = 1 + modrm_length(p + 1);
            set_register(context, reg, size, device.read(address, size));
            break;
        }
        case 0x89:  // mov m, r
        case 0x88: {
            const uint32_t size = opcode == 0x88 ? 1 : operand;
            length = 1 + modrm_length(p + 1);
            device.write(address, size, get_register(context, reg, size));
            break;
        }
        case 0xC7:  // mov m, imm
        case 0xC6: {
            const uint32_t size = opcode == 0xC6 ? 1 : operand;
            const uint32_t operand_length = 1 + modrm_length(p + 1);
            device.write(address, size, immediate(p + operand_length, size));
            length = operand_length + size;
            break;
        }
        case 0xA1:  // mov eax, [moffs]
        case 0xA0:
            set_register(context, 0, opcode == 0xA0 ? 1 : operand, device.read(address, opcode == 0xA0 ? 1 : operand));
            length = 5;
            break;
        case 0xA3:  // mov [moffs], eax
        case 0xA2:
            device.write(address, opcode == 0xA2 ? 1 : operand, get_register(context, 0, opcode == 0xA2 ? 1 : operand));
            length = 5;
            break;
        case 0x0F: {
            const uint8_t second = p[1];
            const uint8_t reg2 = (p[2] >> 3) & 7;
            if (second != 0xB6 && second != 0xB7 && second != 0xBE && second != 0xBF) return false;
            const uint32_t size = (second == 0xB6 || second == 0xBE) ? 1 : 2;
            uint32_t value = device.read(address, size);
            if ((second == 0xBE || second == 0xBF) && (value & sign_bit(size))) value |= ~mask_of(size);
            set_register(context, reg2, operand, value);
            length = 2 + modrm_length(p + 2);
            break;
        }
        case 0x85:  // test m, r
        case 0x84: {
            const uint32_t size = opcode == 0x84 ? 1 : operand;
            logic_flags(context, device.read(address, size) & get_register(context, reg, size), size);
            length = 1 + modrm_length(p + 1);
            break;
        }
        case 0xF7:  // test m, imm
        case 0xF6: {
            if (reg != 0) return false;
            const uint32_t size = opcode == 0xF6 ? 1 : operand;
            const uint32_t operand_length = 1 + modrm_length(p + 1);
            logic_flags(context, device.read(address, size) & immediate(p + operand_length, size), size);
            length = operand_length + size;
            break;
        }
        case 0x3B:  // cmp r, m
        case 0x3A: {
            const uint32_t size = opcode == 0x3A ? 1 : operand;
            compare_flags(context, get_register(context, reg, size), device.read(address, size), size);
            length = 1 + modrm_length(p + 1);
            break;
        }
        case 0x39:  // cmp m, r
        case 0x38: {
            const uint32_t size = opcode == 0x38 ? 1 : operand;
            compare_flags(context, device.read(address, size), get_register(context, reg, size), size);
            length = 1 + modrm_length(p + 1);
            break;
        }
        case 0x0B:  // or r, m
        case 0x23: {  // and r, m
            const uint32_t value = device.read(address, operand);
            const uint32_t result = opcode == 0x0B ? get_register(context, reg, operand) | value
                                                   : get_register(context, reg, operand) & value;
            set_register(context, reg, operand, result);
            logic_flags(context, result, operand);
            length = 1 + modrm_length(p + 1);
            break;
        }
        case 0x09:  // or m, r
        case 0x21:  // and m, r
            rmw(operand, get_register(context, reg, operand), opcode == 0x21);
            length = 1 + modrm_length(p + 1);
            break;
        case 0x81:  // group 1, imm32
        case 0x83:  // group 1, imm8 sign-extended
        case 0x80: {  // group 1, 8-bit
            const uint32_t size = opcode == 0x80 ? 1 : operand;
            const uint32_t operand_length = 1 + modrm_length(p + 1);
            const uint32_t immediate_size = opcode == 0x81 ? size : 1;
            uint32_t value = immediate(p + operand_length, immediate_size);
            if (opcode == 0x83 && (value & 0x80)) value |= 0xFFFFFF00;
            value &= mask_of(size);
            length = operand_length + immediate_size;
            if (reg == 1) {
                rmw(size, value, false);
            } else if (reg == 4) {
                rmw(size, value, true);
            } else if (reg == 7) {
                compare_flags(context, device.read(address, size), value, size);
            } else {
                return false;
            }
            break;
        }
        default:
            return false;
    }
    context->Eip += prefix + length;
    return true;
}

}  // namespace mc3::pc
