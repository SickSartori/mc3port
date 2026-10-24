#include "pc/privileged.h"

#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_map>

#include "pc/log.h"

namespace mc3::pc {
namespace {

std::mutex g_mutex;
// Ports behave as latches: a read returns the last value written. Enough for the read-modify-write sequences
// the libraries do on hardware the port does not have; specific devices can be modelled here when needed.
std::unordered_map<uint16_t, uint32_t> g_ports;
std::set<uint32_t> g_logged;  // (direction << 16) | port

uint32_t port_read(uint16_t port, uint32_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint32_t value = g_ports[port];
    if (g_logged.insert(port).second) log_line("port read  %04X (%u bytes) = %08X", port, size, value);
    return size == 1 ? value & 0xFF : size == 2 ? value & 0xFFFF : value;
}

void port_write(uint16_t port, uint32_t size, uint32_t value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ports[port] = value;
    if (g_logged.insert(0x10000u | port).second) log_line("port write %04X (%u bytes) = %08X", port, size, value);
}

void set_accumulator(CONTEXT* context, uint32_t size, uint32_t value) {
    if (size == 1) {
        context->Eax = (context->Eax & ~0xFFu) | (value & 0xFF);
    } else if (size == 2) {
        context->Eax = (context->Eax & ~0xFFFFu) | (value & 0xFFFF);
    } else {
        context->Eax = value;
    }
}

uint32_t accumulator(const CONTEXT* context, uint32_t size) {
    return size == 1 ? context->Eax & 0xFF : size == 2 ? context->Eax & 0xFFFF : context->Eax;
}

}  // namespace

bool emulate_privileged_instruction(CONTEXT* context) {
    const auto* code = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(context->Eip));
    uint32_t length = 0;
    bool operand_16 = false;
    if (code[0] == 0x66) {
        operand_16 = true;
        ++length;
    }
    const uint8_t opcode = code[length];
    const uint32_t wide = operand_16 ? 2 : 4;
    const uint16_t dx = static_cast<uint16_t>(context->Edx);

    switch (opcode) {
        case 0xEC: set_accumulator(context, 1, port_read(dx, 1)); length += 1; break;                 // in al, dx
        case 0xED: set_accumulator(context, wide, port_read(dx, wide)); length += 1; break;           // in eax, dx
        case 0xEE: port_write(dx, 1, accumulator(context, 1)); length += 1; break;                    // out dx, al
        case 0xEF: port_write(dx, wide, accumulator(context, wide)); length += 1; break;              // out dx, eax
        case 0xE4: set_accumulator(context, 1, port_read(code[length + 1], 1)); length += 2; break;   // in al, imm8
        case 0xE5: set_accumulator(context, wide, port_read(code[length + 1], wide)); length += 2; break;
        case 0xE6: port_write(code[length + 1], 1, accumulator(context, 1)); length += 2; break;      // out imm8, al
        case 0xE7: port_write(code[length + 1], wide, accumulator(context, wide)); length += 2; break;
        case 0xFA:  // cli
        case 0xFB:  // sti: interrupts are emulated by threads; nothing to mask
            length += 1;
            break;
        case 0xF4:  // hlt: wait for an interrupt
            Sleep(0);
            length += 1;
            break;
        case 0x0F:
            switch (code[length + 1]) {
                case 0x08:  // invd
                case 0x09:  // wbinvd: caches are coherent on the host
                case 0x30:  // wrmsr: MTRR and similar setup
                    length += 2;
                    break;
                case 0x32:  // rdmsr
                    context->Eax = 0;
                    context->Edx = 0;
                    length += 2;
                    break;
                default:
                    return false;
            }
            break;
        default:
            return false;
    }
    context->Eip += length;
    return true;
}

}  // namespace mc3::pc
