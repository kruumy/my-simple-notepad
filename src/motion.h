#pragma once

#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace simple_notepad::motion
{
    // frame-rate independent exponential approach; lands on the target once within snap
    [[nodiscard]] inline float approach(float current, float target, float speed, float snap)
    {
        const float next = current + ((target - current) * (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime)));
        return std::abs(target - next) < snap ? target : next;
    }

    [[nodiscard]] inline float ease_out(float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        return 1.0f - ((1.0f - t) * (1.0f - t) * (1.0f - t));
    }
} // namespace simple_notepad::motion
