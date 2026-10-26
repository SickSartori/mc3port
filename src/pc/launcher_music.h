#pragma once

// The launcher's background music (res/launcher_music.mp3, in mc3runtime.dll's resources): decoded with Media
// Foundation on a helper thread, looped on its own XAudio2 engine, faded in and out.

#include <memory>

namespace mc3::pc {

class LauncherMusic {
public:
    LauncherMusic();
    ~LauncherMusic();  // fades out and stops
    // Call every frame: the fade follows `target` (0..1, times the volume setting).
    void update(float dt, float target);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace mc3::pc
