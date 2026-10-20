// PGR4-Recomp application bridge.
//
// PGR4_D3D12_RENDERER=1: Plume/D3D12 native renderer (src/gpu).
// PGR4_NATIVE_RENDERER=1: Vulkan native renderer.
// both 0: SDK Xenos GPU plugin.
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <rex/rex_app.h>
#include <rex/system.h>
#include <rex/ui/window.h>

#include "content/dlc.h"
#include "profiler.h"

#if PGR4_D3D12_RENDERER
#include <rex/ui/imgui_drawer.h>

#include "gpu/imgui_drawer.h"
#include "gpu/video.h"
#elif PGR4_NATIVE_RENDERER
#include "render/graphics_system.h"
#endif

class Pgr4RecompiledApp : public rex::ReXApp {
public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp>
  Create(rex::ui::WindowedAppContext &ctx) {
    return std::unique_ptr<Pgr4RecompiledApp>(new Pgr4RecompiledApp(
        ctx, "pgr4_recompiled", PPCImageConfig));
  }

protected:
  // Title Update #2. Its code is recompiled from TitleUpdate/default.xex with
  // default.xexp beside it (the runtime applies a sibling .xexp at load, so the
  // image matches the generated code), and the folder is also update:\ for
  // the TU's own data. Kept out of the game root so the base default.xex stays
  // unpatched for base builds sharing the same game files.
  static constexpr const char *kTitleUpdateDir = "TitleUpdate";

  void OnConfigurePaths(rex::PathConfig &paths) override {
    pgr4::content::DumpCvarsAndExitIfRequested();
    const auto tu_dir = paths.game_data_root / kTitleUpdateDir;
    if (!pgr4::content::InstallTitleUpdate(paths.game_data_root, tu_dir)) {
      rex::ShowSimpleMessageBox(
          rex::SimpleMessageBoxType::Error,
          "PGR4 needs Title Update #2.\n\nPut the title update package (TU_16L61VP_...) in the "
          "Content folder inside your game folder, or pass --content_root <folder>, then start "
          "the game again. See README.md for details.");
    }
    if (paths.update_data_root.empty()) {
      paths.update_data_root = tu_dir;
    }
  }

  void OnLoadXexImage(std::string &xex_image) override {
    xex_image = std::string("game:\\") + kTitleUpdateDir + "\\default.xex";
  }

#if PGR4_D3D12_RENDERER
  void OnPreSetup(rex::RuntimeConfig &config) override {
    // Null graphics makes Runtime::Setup skip SDK GPU setup; the Plume device
    // is brought up in OnPreLaunchModule (same split as re:Blue).
    config.gpu_plugin.clear();
    config.graphics = nullptr;
  }

  void OnPreLaunchModule() override {
    if (window()) {
      pgr4::gpu::Video::CreateHostDevice(window()->GetNativeWindowHandle());
    }
    InstallOverlayDrawHook();
  }

  // Title id is known once the module is loaded; the guest has not run yet.
  void OnPostLaunchModule(rex::system::XThread *) override {
    pgr4::StartProfiler();
    pgr4::content::InstallDLC();
    pgr4::content::DumpGuestImage();
  }

  // The SDK builds its ImGui overlays (Backtick: console log, F3: debug,
  // F4: settings) on this drawer when the app brings its own renderer.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    return std::make_unique<pgr4::gpu::ImGuiOverlayDrawer>();
  }

  // Present runs on the guest thread and ImGui on the UI thread, so the draw
  // is marshalled synchronously; Present stays parked (holding the frame)
  // while the UI thread records into its command list. Same as re:Blue.
  void InstallOverlayDrawHook() {
    pgr4::gpu::Video::SetOverlayDrawHook(
        [this](plume::RenderCommandList *cmd, plume::RenderFramebuffer *fb, uint32_t w,
               uint32_t h) {
          if (!imgui_drawer() || !imgui_drawer()->HasDialogs()) {
            return;
          }
          app_context().CallInUIThreadSynchronous([this, cmd, fb, w, h] {
            pgr4::gpu::OverlayDrawContext ctx(w, h, cmd, fb);
            imgui_drawer()->Draw(ctx);
          });
        });
  }

  void OnWindowPixelSizeChanged(uint32_t, uint32_t) override {
    pgr4::gpu::Video::RequestResize();
  }

  void OnWindowRestored() override { pgr4::gpu::Video::RequestResize(); }

  void OnShutdown() override { pgr4::gpu::Video::Shutdown(); }

  std::unique_ptr<rex::ui::AchievementNotificationDialog>
  CreateAchievementNotificationDialog() override {
    return nullptr;
  }
#elif PGR4_NATIVE_RENDERER
  void OnPreSetup(rex::RuntimeConfig &config) override {
    auto graphics = std::make_unique<pgr4::render::Pgr4GraphicsSystem>();
    native_graphics_ = graphics.get();
    config.gpu_plugin.clear();
    config.graphics = std::move(graphics);
  }

  void OnPostSetup() override {
    if (!native_graphics_ || !window()) {
      return;
    }
    native_graphics_->AttachWindow(window()->GetNativeWindowHandle());
    native_graphics_->LoadShaderDirectory(
        std::filesystem::current_path() / "pgr4-native-shaders");
  }

  void OnWindowPixelSizeChanged(uint32_t, uint32_t) override {
    if (native_graphics_) {
      native_graphics_->ResizeSwapchain();
    }
  }

  void OnWindowRestored() override {
    if (native_graphics_) {
      native_graphics_->ResizeSwapchain();
    }
  }

  void OnShutdown() override { native_graphics_ = nullptr; }

  std::unique_ptr<rex::ui::AchievementNotificationDialog>
  CreateAchievementNotificationDialog() override {
    return nullptr;
  }

private:
  pgr4::render::Pgr4GraphicsSystem *native_graphics_ = nullptr;
#else
  void OnPreSetup(rex::RuntimeConfig &config) override {
    // Default to Xenos so the build runs without --gpu_plugin.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }
#endif
};
