# hud_mc3port

The pictures of the HUD setting **mc3port** (`hud_style = 1`) that the game itself draws, as PNGs with a palette that
the game colours as it does its own (`external/xport`, `renderer_replacement_folder`), named after the hash of the game
texture each replaces.

- The radar (`4177E4BEB6F9DB13.png`): a dark band with a thin line in the HUD's colour and the N in a disc, like the
  port's dials. The game draws the map, the opponents and the checkpoints on it, so they keep their places.
- Made with `draw_radar_mc3port.py` (sick-xport, `tools/saves`) from the game's texture.
