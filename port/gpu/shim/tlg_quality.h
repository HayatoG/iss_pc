// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: automatic quality for inFAMOUS Second Son (tlg_view.cpp). The game has its own ladder of
// 21 dynamic quality levels (shorter LOD, fewer shadow cascades, fewer objects and lights drawn),
// but it climbs it only when its draw lists run out of memory, not when the frame is slow. This
// governor climbs it by frame time instead: it raises the lowest level the game may use when the
// frame rate stays under the target and gives a level back when there is room to spare.
#pragma once

namespace TlgQuality {

/// The game drops instances at random every frame from level 15 on (0.8% at 15, 3.1% at 16: lights,
/// particles and objects blink), and levels 17 and up never leave the loading screen on the 11% save
/// route (measured). Level 14 had the same frame rate as 16 there, so the governor and the manual floor stopped
/// at 14 at first. 2026-11-23: capped at 12 for the F1 slider, tlg.ini, the governor and the floor. At 13 and 14
/// the game unloads its sharp textures, and after going back to 0 they took minutes to stream in again; at 12
/// they stayed and came back quickly up close (the owner's test, RTX 3080 Ti, 3440x1440).
inline constexpr int MaxLevel = 12;

/// Fed once a second with the frame rate and the draws per frame of that second.
class Governor {
public:
    /// The level the governor wants as the game's lowest, 0..MaxLevel.
    int Update(int target_fps, double fps, double draws_per_frame) {
        if (target_fps <= 0) {
            *this = Governor{};
            return 0;
        }
        since_lowered_ += 1.0;
        raise_cooldown_ -= 1.0;
        lower_cooldown_ -= 1.0;
        blocked_ -= 1.0;
        // Menus, loading screens and cutscenes draw little: their frame rate says nothing about the game.
        if (draws_per_frame < MinGameplayDraws) {
            slow_ = fast_ = 0;
            return level_;
        }
        if (fps < target_fps * SlowFactor) {
            ++slow_;
            fast_ = 0;
        } else if (fps > target_fps * FastFactor) {
            ++fast_;
            slow_ = 0;
        } else {
            slow_ = fast_ = 0;
        }
        if (slow_ >= SlowSeconds && raise_cooldown_ <= 0.0) {
            const int step = fps < target_fps * VerySlowFactor ? 3 : 1;
            level_ = level_ + step < MaxLevel ? level_ + step : MaxLevel;
            raise_cooldown_ = RaiseCooldown;
            lower_cooldown_ = LowerCooldown;
            slow_ = 0;
            // A level we just gave back made the frame slow again: do not try again for a while.
            if (since_lowered_ < FlapSeconds) {
                blocked_ = FlapBlock;
            }
        } else if (fast_ >= FastSeconds && lower_cooldown_ <= 0.0 && blocked_ <= 0.0 && level_ > 0) {
            --level_;
            lower_cooldown_ = LowerCooldown;
            fast_ = 0;
            since_lowered_ = 0.0;
        }
        return level_;
    }

    int Level() const { return level_; }

    static constexpr double MinGameplayDraws = 300.0;
    static constexpr double SlowFactor = 0.93;      ///< under 93% of the target is slow
    static constexpr double VerySlowFactor = 0.75;  ///< under 75% climbs three levels at once
    static constexpr double FastFactor = 1.30;      ///< over 130% of the target has room to spare
    static constexpr int SlowSeconds = 2;
    static constexpr int FastSeconds = 8;
    static constexpr double RaiseCooldown = 3.0;
    static constexpr double LowerCooldown = 6.0;
    static constexpr double FlapSeconds = 10.0;
    static constexpr double FlapBlock = 40.0;

private:
    int level_ = 0;
    int slow_ = 0;
    int fast_ = 0;
    double raise_cooldown_ = 0.0;   ///< after a climb, until the next climb
    double lower_cooldown_ = 0.0;   ///< after any change, until a level may be given back
    double blocked_ = 0.0;
    double since_lowered_ = 1.0e9;
};

} // namespace TlgQuality
