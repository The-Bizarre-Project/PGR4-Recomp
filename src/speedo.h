// pgr4_recompiled - digital speedometer for the in-car view (speedo.cpp)
#pragma once

#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}

namespace pgr4::speedo {

// The app's ImGui drawer and a way to run code on the UI thread, where the speedometer's dialog
// is added and removed (ImGui dialogs belong to the UI thread). Call once, before the title runs.
void Install(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui);

}  // namespace pgr4::speedo
