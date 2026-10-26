#include "pc/settings.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "pc/log.h"

namespace mc3::pc {
namespace {

struct Setting {
    const char* key;
    float value;          // default
    const char* comment;  // written above the key in a new file (Italian: the file is for the player)
};

const Setting kSettings[] = {
    {"frame_rate", 30, "Fotogrammi al secondo del gioco: 30 come l'Xbox, 60 piu' fluido (serve un PC che regga il doppio del lavoro)."},
    {"widescreen", 1, "Schermo panoramico 16:9: 1 = il gioco usa la sua modalita' panoramica (piu' campo visivo, niente bande nere), 0 = 4:3 come un televisore classico."},
    {"gpu_vertices", 1, "Calcolo dei vertici sulla scheda video: 1 = si' (piu' veloce), 0 = sul processore come prima."},
    {"render_scale", 2, "Risoluzione interna: 1 = come l'Xbox (640x480), 2 = 1280x960, 3, 4. Piu' alta = bordi piu' lisci."},
    {"anisotropy", 16, "Filtro anisotropico delle texture: 1 = spento, fino a 16."},
    {"effects", 1, "Effetti moderni: 1 = attivi, 0 = immagine originale (F10 li accende e spegne durante il gioco)."},
    {"sharpen", 0.5f, "Nitidezza adattiva: 0 = spenta, 1 = massima."},
    {"bloom", 0.35f, "Bagliore delle luci (fari, insegne, lampioni): 0 = spento, 1 = forte."},
    {"bloom_threshold", 0.7f, "Da quale luminosita' una zona produce bagliore: 0 = tutto, 1 = solo il bianco pieno."},
    {"saturation", 1.12f, "Saturazione dei colori: 1 = originale, piu' alta = colori piu' vivi."},
    {"contrast", 1.06f, "Contrasto: 1 = originale."},
    {"vignette", 0.15f, "Leggero scurimento dei bordi dell'immagine: 0 = spento."},
    {"ambient_occlusion", 0.6f, "Ombreggiatura ambientale (angoli e contatti piu' scuri): 0 = spenta, 1 = forte."},
    {"ambient_occlusion_radius", 1, "Ampiezza dell'ombreggiatura ambientale: 0.5 = stretta, 2 = larga."},
};

std::map<std::string, float> g_values;

std::string environment_name(const char* key) {
    std::string name = "MC3_";
    for (const char* c = key; *c; ++c) name += static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    return name;
}

void write_defaults(const std::filesystem::path& file) {
    std::ofstream out(file);
    if (!out) return;
    out << "# Impostazioni di mc3port. Modifica i valori e riavvia il gioco.\n"
           "# Ogni voce si puo' anche dare come variabile d'ambiente MC3_<NOME> (per esempio MC3_BLOOM=0).\n\n";
    for (const Setting& s : kSettings) {
        char value[32];
        std::snprintf(value, sizeof(value), "%g", s.value);
        out << "# " << s.comment << "\n" << s.key << " = " << value << "\n\n";
    }
}

}  // namespace

void settings_load(const std::filesystem::path& file) {
    for (const Setting& s : kSettings) g_values[s.key] = s.value;
    if (!std::filesystem::exists(file)) {
        write_defaults(file);
        log_line("settings: %s written with the defaults", file.string().c_str());
    }
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string key = line.substr(0, equals);
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t\r") + 1);
        if (g_values.count(key)) g_values[key] = static_cast<float>(std::atof(line.c_str() + equals + 1));
    }
    for (auto& [key, value] : g_values) {
        char text[32] = {};
        if (GetEnvironmentVariableA(environment_name(key.c_str()).c_str(), text, sizeof(text))) {
            value = static_cast<float>(std::atof(text));
        }
        log_line("settings: %s = %g", key.c_str(), value);
    }
}

float setting(const char* key) {
    const auto found = g_values.find(key);
    if (found != g_values.end()) return found->second;
    for (const Setting& s : kSettings) {
        if (std::strcmp(s.key, key) == 0) return s.value;
    }
    return 0;
}

}  // namespace mc3::pc
