#include "pc/hud.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <set>
#include <vector>

#include "imgui.h"
#include "pc/log.h"
#include "pc/renderer_d3d11.h"
#include "pc/settings.h"

namespace mc3::pc {
namespace {

constexpr float kPi = 3.14159265f;

// What one frame's draws showed, per player (two in split screen).
struct Dial {
    bool seen = false;
    float left = 0, top = 0, right = 0, bottom = 0;
    float angle = 0;  // the needle, degrees clockwise from straight up
    bool has_needle = false;
    float centre_x() const { return (left + right) / 2; }
    float centre_y() const { return (top + bottom) / 2; }
    float size() const { return bottom - top; }
};
struct Player {
    Dial speed, rpm;
    int gear = 0;  // 1-6; 0 none seen, -1 reverse, -2 neutral, -3 a cell not known yet
    float special = -1;  // the special ability's charge, 0 to 1; -1 none seen
    float nitro = -1;    // what is left of the nitrous, 0 to 1; -1 none seen (a car without it)
    float nitro_colour[3] = {};
};
struct Reading {
    std::vector<Dial> faces;  // in draw order, with their textures
    std::vector<uint32_t> face_textures;
    std::vector<std::pair<float, float>> gears;  // (x, y) of the digit
    std::vector<int> gear_values;
    std::vector<HudBox> gear_circles;
    struct Arc {
        uint32_t texture;
        float left, right, top, bottom, fill_top, fill_bottom;
        bool filled;
    };
    std::vector<Arc> arcs;
    struct Segment {
        float x, y;
    };
    std::vector<Segment> segments;
    struct Nitro {
        float left, right, bottom, height;
        float colour[3];
    };
    std::vector<Nitro> nitros;
    std::vector<std::pair<uint32_t, float>> early_needles;  // (texture, angle): drawn before their face
};
Reading g_reading;
std::vector<Player> g_players;          // the last shown frame's
std::vector<HudBox> g_boxes;            // and where its dials were
std::set<uint32_t> g_gear_textures;     // seen with a gear digit: their other quads (the circle) are the gear's too
std::set<uint32_t> g_dial_textures;     // seen with a needle: their squares are dial faces
std::set<uint32_t> g_logged_cells;

// Every corner shares its x with another corner and its y with another: not turned.
bool axis_aligned(const HudQuad& q) {
    for (int i = 0; i < 4; ++i) {
        bool same_x = false, same_y = false;
        for (int j = 0; j < 4; ++j) {
            if (j == i) continue;
            same_x |= std::fabs(q.x[i] - q.x[j]) < 0.5f;
            same_y |= std::fabs(q.y[i] - q.y[j]) < 0.5f;
        }
        if (!same_x || !same_y) return false;
    }
    return true;
}

void bounds(const HudQuad& q, float& left, float& top, float& right, float& bottom) {
    left = *std::min_element(q.x, q.x + 4), right = *std::max_element(q.x, q.x + 4);
    top = *std::min_element(q.y, q.y + 4), bottom = *std::max_element(q.y, q.y + 4);
}

// The gear digit's cell in the game's atlas (texture coordinates of its top left corner) to the gear. Read from
// race frames; a cell not in the table is logged once.
int gear_of_cell(float u, float v) {
    struct Cell {
        float u, v;
        int gear;
    };
    // Reverse and neutral sit before 1 and 2 in their row (seen in play, not yet matched with a picture).
    static const Cell kCells[] = {{0.336f, 0.852f, -1}, {0.5f, 0.852f, -2},  {0.664f, 0.852f, 1}, {0.828f, 0.852f, 2},
                                  {0.336f, 0.703f, 3},  {0.5f, 0.703f, 4},   {0.664f, 0.703f, 5}, {0.828f, 0.703f, 6}};
    for (const Cell& c : kCells) {
        if (std::fabs(c.u - u) < 0.02f && std::fabs(c.v - v) < 0.02f) return c.gear;
    }
    const uint32_t key = static_cast<uint32_t>(u * 1000) << 16 | static_cast<uint32_t>(v * 1000);
    if (g_logged_cells.insert(key).second) log_line("hud: gear digit cell %.3f %.3f not known yet", u, v);
    return -3;
}

// The nitrous gauge: one quad up and to the right of the speedometer (77,-47 and 80 x 40 on a face of 108), whose top
// comes down as the nitrous burns, to a height of 0, still drawn. Only a car with nitrous has it.
constexpr float kNitroLeft = 77.0f / 108, kNitroBottom = -7.0f / 108, kNitroWidth = 80.0f / 108, kNitroHeight = 40.0f / 108;
float g_nitro_full = 0;          // the tallest seen since the dials last showed
// The HUD colour the player chose in the garage: the game tints only the nitrous gauge (and the radar's ring) with it,
// so it is read there and kept for cars without nitrous. Orange until one is seen, the game's default.
float g_hud_colour[3] = {1.0f, 0.54f, 0.0f};
uint32_t g_nitro_texture = 0;    // its texture: for the log, other pieces on it

// One frame's reading, complete: its players replace the last ones (a reading without a speedometer and its needle,
// a frame cut short, changes nothing).
void publish(Reading& r) {
    // Players: one per speedometer (the larger faces with a needle), each taking the nearest of the rest.
    std::vector<Player> players;
    float largest = 0;
    for (const Dial& face : r.faces) {
        if (face.has_needle) largest = std::max(largest, face.size());
    }
    for (const Dial& face : r.faces) {
        if (face.has_needle && face.size() > largest * 0.8f) {
            Player p;
            p.speed = face;
            players.push_back(p);
        }
    }
    const auto nearest = [&](float x, float y) -> Player* {
        Player* best = nullptr;
        float best_distance = 1e9f;
        for (Player& p : players) {
            const float d = std::hypot(p.speed.centre_x() - x, p.speed.centre_y() - y);
            if (d < best_distance) best_distance = d, best = &p;
        }
        return best;
    };
    for (const Dial& face : r.faces) {
        if (!face.has_needle || face.size() > largest * 0.8f) continue;
        if (Player* p = nearest(face.centre_x(), face.centre_y())) p->rpm = face;
    }
    for (size_t i = 0; i < r.gears.size(); ++i) {
        if (Player* p = nearest(r.gears[i].first, r.gears[i].second)) p->gear = r.gear_values[i];
    }
    for (const Reading::Arc& arc : r.arcs) {
        if (Player* p = nearest((arc.left + arc.right) / 2, arc.bottom)) {
            const float span = std::max(1.0f, arc.fill_bottom - arc.top);
            p->special = arc.filled ? std::clamp((arc.fill_bottom - arc.fill_top) / span, 0.0f, 1.0f) : 0;
        }
    }
    for (const Reading::Nitro& nitro : r.nitros) {
        if (Player* p = nearest((nitro.left + nitro.right) / 2, nitro.bottom)) {
            // Relative to the fullest seen on this car: upgrades give it more, and 40 lines are only what was measured.
            g_nitro_full = std::max(g_nitro_full, nitro.height);
            p->nitro = g_nitro_full > 0 ? std::clamp(nitro.height / g_nitro_full, 0.0f, 1.0f) : 0;
            std::copy(nitro.colour, nitro.colour + 3, p->nitro_colour);
            std::copy(nitro.colour, nitro.colour + 3, g_hud_colour);
        }
    }
    // A reading whose largest dial is much smaller than the last speedometer missed the speedometer (its rev
    // counter would pass for one): the last reading stays.
    const bool smaller = !g_players.empty() && largest < g_players.front().speed.size() * 0.8f;
    // For a report: the first readings that change what is shown in an unusual way (none at all, a smaller dial,
    // another place), with what they held.
    static int reports = 40;
    const bool moved = !players.empty() && !g_players.empty() &&
                       (std::fabs(players.front().speed.left - g_players.front().speed.left) > 2 ||
                        std::fabs(players.front().speed.top - g_players.front().speed.top) > 2);
    if (reports > 0 && (players.empty() || smaller || moved || players.size() != g_players.size())) {
        --reports;
        log_line("hud: reading of %zu faces, %zu arcs, %zu segments, %zu gears: %zu players%s%s (speedometer %.0f,%.0f "
                 "size %.0f)", r.faces.size(), r.arcs.size(), r.segments.size(), r.gears.size(), players.size(),
                 smaller ? ", smaller, left out" : "", moved ? ", moved" : "",
                 players.empty() ? 0.0f : players.front().speed.left,
                 players.empty() ? 0.0f : players.front().speed.top,
                 largest);
    }
    if (!players.empty() && !smaller) {
        g_players = std::move(players);
        g_boxes.clear();
        for (const Dial& face : r.faces) {
            if (face.has_needle) g_boxes.push_back({face.left, face.top, face.right, face.bottom});
        }
        g_boxes.insert(g_boxes.end(), r.gear_circles.begin(), r.gear_circles.end());
        for (const Reading::Arc& arc : r.arcs) g_boxes.push_back({arc.left, arc.top, arc.right, arc.bottom});
        for (const Reading::Nitro& nitro : r.nitros) {
            g_boxes.push_back({nitro.left, nitro.bottom - largest * kNitroHeight, nitro.right, nitro.bottom});
        }
        if (!r.segments.empty()) {
            HudBox bar{1e9f, 1e9f, -1e9f, -1e9f};
            for (const Reading::Segment& s : r.segments) {
                bar = {std::min(bar.left, s.x - 12), std::min(bar.top, s.y - 12), std::max(bar.right, s.x + 12),
                       std::max(bar.bottom, s.y + 12)};
            }
            g_boxes.push_back(bar);
        }
    }
    // A needle drawn before its face belongs to the frame that face starts.
    std::vector<std::pair<uint32_t, float>> early = std::move(r.early_needles);
    r = Reading{};
    r.early_needles = std::move(early);
}

// The game draws its HUD again: the same piece in the same place as one already read starts the next frame.
bool starts_next_frame(const Reading& r, uint32_t texture, float left, float top) {
    for (size_t i = 0; i < r.faces.size(); ++i) {
        if (r.face_textures[i] == texture && std::fabs(r.faces[i].left - left) < 1 &&
            std::fabs(r.faces[i].top - top) < 1) {
            return true;
        }
    }
    for (const Reading::Arc& arc : r.arcs) {
        if (arc.texture == texture && std::fabs(arc.left - left) < 1 && std::fabs(arc.top - top) < 1) return true;
    }
    return false;
}

int g_presents_without = 0;  // presents since the last dial was read

}  // namespace

bool hud_enabled() {
    static const bool enabled = setting_at_start("hud_style") != 0;
    return enabled;
}

bool hud_reading() {
    static const bool reading =
        hud_enabled() || (setting_at_start("widescreen") != 0 && setting_at_start("hud_aspect") != 0);
    return reading;
}

const std::vector<HudBox>& hud_boxes() { return g_boxes; }

bool hud_take_quad(const HudQuad& q) {
    if (!hud_reading()) return false;
    const bool hide = hud_enabled();
    float left, top, right, bottom;
    bounds(q, left, top, right, bottom);
    const float width = right - left, height = bottom - top;
    const float u0 = std::min({q.u[0], q.u[1], q.u[2], q.u[3]}), v0 = std::min({q.v[0], q.v[1], q.v[2], q.v[3]});
    const float u1 = std::max({q.u[0], q.u[1], q.u[2], q.u[3]}), v1 = std::max({q.v[0], q.v[1], q.v[2], q.v[3]});
    const bool aligned = axis_aligned(q);
    Reading& r = g_reading;

    if (q.program && aligned) {
        // The special ability: a tall track, then its fill on the same texture and columns (height 0 when empty).
        // The fill comes first here: once the charge passes a third it is tall too, and was read as a new track
        // starting the next frame, so a charged ability showed as empty.
        if (!r.arcs.empty()) {
            Reading::Arc& arc = r.arcs.back();
            if (arc.texture == q.texture && std::fabs(arc.left - left) < 1 && std::fabs(arc.right - right) < 1 &&
                !arc.filled) {
                arc.fill_top = top, arc.fill_bottom = bottom, arc.filled = true;
                return hide;
            }
        }
        if (starts_next_frame(r, q.texture, left, top)) publish(r);
        for (const Player& p : g_players) {
            const float size = p.speed.size();
            if (std::fabs(left - p.speed.left - kNitroLeft * size) < 0.05f * size &&
                std::fabs(bottom - p.speed.top - kNitroBottom * size) < 0.05f * size &&
                std::fabs(width - kNitroWidth * size) < 0.05f * size) {
                g_nitro_texture = q.texture;
                r.nitros.push_back({left, right, bottom, height, {q.colour[0], q.colour[1], q.colour[2]}});
                return hide;
            }
        }
        if (height > 3 * width && width >= 20) {
            r.arcs.push_back({q.texture, left, right, top, bottom, bottom, bottom, false});
            g_presents_without = 0;
            return hide;
        }
        // shortcut: only the one gauge piece is known, from a car with little nitrous. Upgraded cars may draw others:
        // they stay where the game puts them, and their first shapes go to the log to be recognised.
        if (q.texture == g_nitro_texture && !g_players.empty()) {
            static int reports = 12;
            if (reports > 0) {
                --reports;
                const Dial& speed = g_players.front().speed;
                log_line("hud: another piece on the nitrous texture: %.0fx%.0f at %.0f,%.0f, colour %.2f %.2f %.2f, uv %.3f,%.3f-%.3f,%.3f",
                         width, height, left - speed.left, top - speed.top, q.colour[0], q.colour[1], q.colour[2], u0, v0, u1, v1);
            }
        }
        // The gear: its digit, one cell of an atlas (0.164 x 0.148 of the texture), inside the circle just drawn on
        // the same texture. Without the circle it is a letter of some text, not the gear.
        if (std::fabs(u1 - u0 - 0.164f) < 0.01f && std::fabs(v1 - v0 - 0.148f) < 0.01f && q.colour[3] > 0.9f &&
            !r.faces.empty() && r.face_textures.back() == q.texture) {
            const Dial& circle = r.faces.back();
            const float x = (left + right) / 2, y = (top + bottom) / 2;
            if (x > circle.left && x < circle.right && y > circle.top && y < circle.bottom) {
                g_gear_textures.insert(q.texture);
                r.gear_circles.push_back({circle.left, circle.top, circle.right, circle.bottom});
                r.faces.pop_back();
                r.face_textures.pop_back();
                r.gears.push_back({x, y});
                r.gear_values.push_back(gear_of_cell(u0, v0));
                return hide;
            }
        }
        // A dial's face (or the gear's circle): a square; a needle follows on the same texture (or the digit).
        // Left out once a needle or a digit has shown what the texture is (from the next frame), so that no other
        // square goes missing.
        if (std::fabs(width - height) < 0.12f * std::max(width, height) && width >= 30) {
            Dial face;
            face.seen = true, face.left = left, face.top = top, face.right = right, face.bottom = bottom;
            for (size_t i = 0; i < r.early_needles.size(); ++i) {
                if (r.early_needles[i].first == q.texture) {
                    face.angle = r.early_needles[i].second, face.has_needle = true;
                    r.early_needles.erase(r.early_needles.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            r.faces.push_back(face);
            r.face_textures.push_back(q.texture);
            return hide && (g_dial_textures.count(q.texture) != 0 || g_gear_textures.count(q.texture) != 0);
        }
        return false;
    }
    if (!q.program) {
        // A needle: a quad on a face's texture, turned (or not: straight up, it is not). Its direction, from the end
        // with the lower texture coordinate to the other, in surface pixels (the game turns it there). The game
        // draws it after its face, or in some frames before: then it waits for the face (and is left out once the
        // texture is known).
        const auto direction = [&] {
            float tip_x = 0, tip_y = 0, tail_x = 0, tail_y = 0;
            const float middle = (v0 + v1) / 2;
            for (int k = 0; k < 4; ++k) {
                if (q.v[k] > middle) {
                    tip_x += q.x[k] / 2, tip_y += q.y[k] / 2;
                } else {
                    tail_x += q.x[k] / 2, tail_y += q.y[k] / 2;
                }
            }
            return std::atan2(tip_x - tail_x, tail_y - tip_y) * 180 / kPi;
        };
        for (size_t i = r.faces.size(); i-- > 0;) {
            if (r.face_textures[i] != q.texture || r.faces[i].has_needle) continue;
            r.faces[i].angle = direction();
            r.faces[i].has_needle = true;
            g_dial_textures.insert(q.texture);
            g_presents_without = 0;
            return hide;
        }
        if (g_dial_textures.count(q.texture)) {
            r.early_needles.push_back({q.texture, direction()});
            g_presents_without = 0;
            return hide;
        }
    }
    if (!q.program && q.colour[0] > 0.6f && q.colour[1] < 0.2f && q.colour[2] < 0.2f && width < 40 && height < 40) {
        // The red bar under the speedometer (the rev limiter): ten small segments around a speedometer already read
        // (a small red letter elsewhere stays). Left out with the dials.
        const float x = (left + right) / 2, y = (top + bottom) / 2;
        const bool near_speedometer = std::any_of(g_players.begin(), g_players.end(), [&](const Player& p) {
            return std::hypot(p.speed.centre_x() - x, p.speed.centre_y() - y) < p.speed.size() * 0.75f;
        });
        if (!near_speedometer) return false;
        r.segments.push_back({x, y});
        return hide;
    }
    return false;
}

void hud_frame_shown() {
    // A menu or a loading screen: no dials for a while.
    if (hud_reading() && ++g_presents_without > 30 && !g_players.empty()) {
        g_players.clear();
        g_boxes.clear();
        g_nitro_full = 0;  // perhaps another car
        log_line("hud: no dials for 30 presents, the HUD is hidden");
    }
}

namespace {

ImU32 colour(int r, int g, int b, float a = 1) {
    return IM_COL32(r, g, b, static_cast<int>(std::clamp(a, 0.0f, 1.0f) * 255));
}

ImVec2 polar(ImVec2 c, float radius, float degrees) {
    const float a = degrees * kPi / 180;
    return ImVec2(c.x + std::sin(a) * radius, c.y - std::cos(a) * radius);
}

void arc(ImDrawList* list, ImVec2 c, float radius, float from, float to, ImU32 col, float thickness) {
    if (to <= from) return;
    const int segments = std::max(4, static_cast<int>((to - from) / 4));
    // ImGui's angles: 0 = right, clockwise; ours: 0 = up, clockwise.
    list->PathArcTo(c, radius, (from - 90) * kPi / 180, (to - 90) * kPi / 180, segments);
    list->PathStroke(col, ImDrawFlags_None, thickness);
}

void centred_text(ImDrawList* list, ImFont* font, float size, ImVec2 centre, ImU32 col, const char* text) {
    const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    list->AddText(font, size, ImVec2(centre.x - extent.x / 2, centre.y - extent.y / 2), col, text);
}

// One round dial of style A: face, ticks and numbers over 270 degrees, the value's arc, an optional red zone, the
// needle and its cap. `s` is the design's scale (1 = 720 lines).
void dial(ImDrawList* list, const HudFonts& fonts, ImVec2 c, float radius, float s, float value, float maximum,
          float step, float major_every, float red_from, ImU32 accent, bool large) {
    list->AddCircleFilled(c, radius, colour(11, 11, 13), 64);
    list->AddCircleFilled(c, radius * 0.97f, colour(20, 21, 24), 64);
    list->AddCircleFilled(ImVec2(c.x, c.y - radius * 0.18f), radius * 0.62f, colour(43, 44, 49, 0.35f), 64);
    list->AddCircle(c, radius, colour(255, 255, 255, 0.18f), 64, 2 * s);
    const float fraction = std::clamp(value / maximum, 0.0f, 1.0f);
    const float ring = radius - (large ? 9 : 5) * s, ring_width = (large ? 6 : 4) * s;
    const float value_angle = -135 + 270 * fraction;
    const float red_angle = red_from > 0 ? -135 + 270 * red_from / maximum : 135;
    arc(list, c, ring, -135, value_angle, accent, ring_width);
    arc(list, c, ring, value_angle, red_angle, colour(255, 255, 255, 0.10f), ring_width);
    if (red_from > 0) arc(list, c, ring, std::max(value_angle, red_angle), 135, colour(232, 57, 47), ring_width);
    const float inset = (large ? 12 : 7) * s;
    for (float v = 0; v <= maximum + 1e-3f; v += step) {
        const float a = -135 + 270 * v / maximum;
        const bool major = std::fabs(v / major_every - std::round(v / major_every)) < 1e-3f;
        const float length = (major ? (large ? 14 : 8) : (large ? 7 : 4)) * s;
        list->AddLine(polar(c, radius - inset, a), polar(c, radius - inset - length, a),
                      major ? colour(242, 242, 242) : colour(255, 255, 255, 0.5f),
                      (major ? 3 : 2) * s * (large ? 1 : 0.8f));
        if (major) {
            char label[8];
            std::snprintf(label, sizeof(label), "%.0f", v);
            const float label_radius = radius - (large ? 40 : 24) * s;
            centred_text(list, fonts.numbers, (large ? 17 : 11) * s, polar(c, label_radius, a), colour(244, 244, 244),
                         label);
        }
    }
    const float needle = (large ? 110 : 50) * s * radius / (large ? 118 * s : 54 * s);
    const ImVec2 tip = polar(c, needle * 0.92f, value_angle), tail = polar(c, needle * 0.12f, value_angle + 180);
    list->AddLine(tail, tip, (accent & 0x00FFFFFF) | 0x50000000, (large ? 10 : 7) * s);
    list->AddLine(tail, tip, accent, (large ? 4 : 3) * s);
    list->AddCircleFilled(c, (large ? 13 : 7) * s, colour(17, 17, 17), 32);
    list->AddCircle(c, (large ? 13 : 7) * s, accent, 32, (large ? 3 : 2) * s);
}

}  // namespace

void hud_draw(ImDrawList* list, const HudFonts& fonts) {
    if (!hud_enabled() || g_players.empty()) return;
    const HudPicture pic = renderer_hud_picture();
    if (pic.surface_width <= 0 || pic.window_width <= 0) return;
    const float scale_x = pic.window_width / pic.surface_width, scale_y = pic.window_height / pic.surface_height;
    // Where a surface pixel shows: with hud_aspect the game's HUD is narrowed towards the right (renderer_d3d11.cpp).
    const float full_width = pic.surface_width + 2 * pic.surface_left;
    const float full_height = pic.surface_height + 2 * pic.surface_top;
    const float narrow =
        setting_at_start("hud_aspect") != 0 ? std::min(1.0f, full_width / full_height / renderer_aspect()) : 1;
    const auto to_window = [&](float x, float y) {
        x = full_width - (full_width - x) * narrow;
        return ImVec2(pic.window_left + (x - pic.surface_left) * scale_x,
                      pic.window_top + (y - pic.surface_top) * scale_y);
    };
    const bool kmh = setting("hud_units") != 0;
    const auto channel = [](float c) { return static_cast<int>(std::clamp(c, 0.0f, 1.0f) * 255); };
    const int red = channel(g_hud_colour[0]), green = channel(g_hud_colour[1]), blue = channel(g_hud_colour[2]);
    const ImU32 accent = colour(red, green, blue);
    for (const Player& p : g_players) {
        // Style A's layout (1280 x 720), around the speedometer: s = 1 when the game's face is 108 lines of 480.
        const float s = p.speed.size() * scale_y / 162 * 0.85f;  // 0.85: style A at full size covered too much
        const ImVec2 original = to_window(p.speed.centre_x(), p.speed.centre_y());
        // Where the game's speedometer was, kept inside the picture with style A's margin below it.
        const float lowest = pic.window_top + pic.window_height - 124 * s;
        const ImVec2 speed_centre(original.x - 16 * s, std::min(original.y - 17 * s, lowest));
        const float mph = std::max(0.0f, (p.speed.angle + 130) / 270 * 175);
        const float shown = kmh ? mph * 1.609344f : mph;
        dial(list, fonts, speed_centre, 118 * s, s, shown, kmh ? 300.0f : 180.0f, 10, kmh ? 50.0f : 20.0f, 0, accent,
             true);
        char number[16];
        std::snprintf(number, sizeof(number), "%.0f", shown);
        const ImVec2 number_at(speed_centre.x, speed_centre.y + 62 * s);
        centred_text(list, fonts.numbers, 34 * s, number_at, colour(244, 244, 244), number);
        centred_text(list, fonts.labels, 12 * s, ImVec2(speed_centre.x, speed_centre.y + 86 * s), accent,
                     kmh ? "KM/H" : "MPH");

        if (p.rpm.seen) {
            const float rpm = std::max(0.0f, (p.rpm.angle + 130) / 270 * 6);
            const ImVec2 c(speed_centre.x + 41 * s, speed_centre.y - 183 * s);
            dial(list, fonts, c, 54 * s, s, rpm, 7, 0.5f, 1, 6, accent, false);
            centred_text(list, fonts.labels, 9 * s, ImVec2(c.x, c.y + 33 * s), colour(201, 201, 201), "x1000 RPM");
        }
        if (p.gear != 0) {
            const ImVec2 c(speed_centre.x + 122 * s, speed_centre.y - 104 * s);
            list->AddCircleFilled(ImVec2(c.x, c.y + 3 * s), 27 * s, colour(0, 0, 0, 0.45f), 32);
            list->AddCircleFilled(c, 26 * s, accent, 32);
            list->AddCircle(c, 26 * s, colour(11, 11, 13), 32, 3 * s);
            const char* text = p.gear == -1 ? "R" : p.gear == -2 ? "N" : p.gear == -3 ? "-" : nullptr;
            char digit[4];
            if (!text) std::snprintf(digit, sizeof(digit), "%d", p.gear), text = digit;
            centred_text(list, fonts.numbers, 34 * s, ImVec2(c.x - 1 * s, c.y), colour(17, 17, 17), text);
        }
        if (p.nitro >= 0) {  // the nitrous: an arc outside the speedometer, upper right, between rev counter and gear
            const float r = 127 * s;
            arc(list, speed_centre, r, 18, 50, colour(255, 255, 255, 0.16f), 5 * s);
            arc(list, speed_centre, r, 18, 18 + 32 * p.nitro,
                colour(channel(p.nitro_colour[0]), channel(p.nitro_colour[1]), channel(p.nitro_colour[2])), 5 * s);
        }
        // The game's red bar under the speedometer is not redrawn: it lights only at the rev limiter, which the rev
        // counter's red zone already shows.
        if (p.special > 0) {  // the special ability: an arc outside the speedometer, lower right, filling upwards;
                              // none while it is empty (a car without an ability never charges it)
            const float r = 127 * s;
            arc(list, speed_centre, r, 60, 140, colour(255, 255, 255, 0.16f), 5 * s);
            arc(list, speed_centre, r, 140 - 80 * p.special, 140, accent, 5 * s);
            if (p.special > 0.98f) {  // full, ready to use: a glow that pulses twice a second, as the game's own
                const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4 * kPi);
                arc(list, speed_centre, r, 60, 140, colour(red, green, blue, 0.15f + 0.30f * pulse), (9 + 6 * pulse) * s);
                arc(list, speed_centre, r, 60, 140,
                    colour((red + 255) / 2, (green + 255) / 2, (blue + 255) / 2, 0.55f + 0.45f * pulse), 5 * s);
            }
        }
    }
}

}  // namespace mc3::pc
