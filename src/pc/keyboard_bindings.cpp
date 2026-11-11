#include "pc/keyboard_bindings.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "pc/settings.h"

namespace xport::pc {
namespace {

const KeyBinding kBindings[kPadInputs] = {
    {PadInput::kRightTrigger, "rt", "RT", "Accelerate", "Accelera", "Acelerar", "Acelerar", 'W'},
    {PadInput::kLeftTrigger, "lt", "LT", "Brake / reverse", "Frena / retromarcia", "Frear / ré", "Frenar / marcha atrás", 'S'},
    {PadInput::kSteerLeft, "steer_left", "L \xE2\x86\x90", "Steer left", "Sterza a sinistra", "Virar à esquerda", "Girar a la izquierda", 'A'},
    {PadInput::kSteerRight, "steer_right", "L \xE2\x86\x92", "Steer right", "Sterza a destra", "Virar à direita", "Girar a la derecha", 'D'},
    {PadInput::kA, "a", "A", "Handbrake", "Freno a mano", "Freio de mão", "Freno de mano", VK_SPACE},
    {PadInput::kB, "b", "B", "Weight transfer", "Trasferimento peso", "Transferência de peso", "Transferencia de peso", VK_ESCAPE},
    {PadInput::kX, "x", "X", "Nitrous", "Nitro", "Nitro", "Nitro", 'Z'},
    {PadInput::kY, "y", "Y", "Look back", "Guarda indietro", "Olhar para trás", "Mirar atrás", 'C'},
    {PadInput::kLeftThumb, "ls", "LS", "Special ability", "Abilit\xC3\xA0 speciale", "Habilidade especial", "Habilidad especial", 'F'},
    {PadInput::kBlack, "black", "Black", "Change camera", "Cambia telecamera", "Trocar câmera", "Cambiar cámara", VK_LSHIFT},
    {PadInput::kWhite, "white", "White", "Flash headlights", "Lampeggia i fari", "Piscar os faróis", "Ráfaga de luces", VK_LCONTROL},
    {PadInput::kStart, "start", "Start", "Pause", "Pausa", "Pausa", "Pausa", VK_RETURN},
    {PadInput::kBack, "back", "Back", "Navigation system", "Navigatore", "Navegador", "Navegador", VK_BACK},
    {PadInput::kDpadUp, "up", "D-pad \xE2\x86\x91", "Map", "Mappa", "Mapa", "Mapa", VK_UP},
    {PadInput::kDpadDown, "down", "D-pad \xE2\x86\x93", "Hydraulics", "Idrauliche", "Hidráulica", "Hidráulica", VK_DOWN},
    {PadInput::kDpadLeft, "left", "D-pad \xE2\x86\x90", "Previous track", "Brano precedente", "Música anterior", "Canción anterior", VK_LEFT},
    {PadInput::kDpadRight, "right", "D-pad \xE2\x86\x92", "Next track", "Brano successivo", "Próxima música", "Canción siguiente", VK_RIGHT},
    {PadInput::kLookLeft, "look_left", "R \xE2\x86\x90", "Camera left", "Telecamera sinistra", "Câmera à esquerda", "Cámara a la izquierda", 'J'},
    {PadInput::kLookRight, "look_right", "R \xE2\x86\x92", "Camera right", "Telecamera destra", "Câmera à direita", "Cámara a la derecha", 'L'},
    {PadInput::kLookUp, "look_up", "R \xE2\x86\x91", "Camera up", "Telecamera su", "Câmera para cima", "Cámara arriba", 'I'},
    {PadInput::kLookDown, "look_down", "R \xE2\x86\x93", "Camera down", "Telecamera gi\xC3\xB9", "Câmera para baixo", "Cámara abajo", 'K'},
};

std::atomic<int> g_keys[kPadInputs];  // read by the input thread

void save() {
    std::string text;
    for (const KeyBinding& b : kBindings) {
        char item[48];
        std::snprintf(item, sizeof(item), "%s%s:%d", text.empty() ? "" : " ", b.id, g_keys[static_cast<int>(b.input)].load());
        text += item;
    }
    setting_save_text("keyboard", text);
}

}  // namespace

const KeyBinding& key_binding(PadInput input) { return kBindings[static_cast<int>(input)]; }

int bound_key(PadInput input) { return g_keys[static_cast<int>(input)].load(); }

void bind_key(PadInput input, int virtual_key) {
    // A key does one thing: whatever had it before gets the key this input had.
    const int previous = bound_key(input);
    for (std::atomic<int>& key : g_keys) {
        if (key.load() == virtual_key) key = previous;
    }
    g_keys[static_cast<int>(input)] = virtual_key;
    save();
}

void reset_key_bindings() {
    for (const KeyBinding& b : kBindings) g_keys[static_cast<int>(b.input)] = b.default_key;
    save();
}

void load_key_bindings() {
    for (const KeyBinding& b : kBindings) g_keys[static_cast<int>(b.input)] = b.default_key;
    std::istringstream items(setting_text("keyboard"));
    for (std::string item; items >> item;) {
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        const std::string id = item.substr(0, colon);
        const int key = std::atoi(item.c_str() + colon + 1);
        for (const KeyBinding& b : kBindings) {
            if (id == b.id && key > 0 && key < 256) g_keys[static_cast<int>(b.input)] = key;
        }
    }
}

}  // namespace xport::pc
