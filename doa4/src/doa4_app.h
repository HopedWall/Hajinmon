// doa4 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/filesystem/vfs.h>
#include <rex/logging/macros.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "cheats.h"
#include "crash_trace.h"
#include "frame_capture.h"
#include "input_defaults.h"
#include "renderer_select.h"
#include "save_trace.h"
#include "user_data.h"

class Doa4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    // The app name is what players see: the window title and the
    // Documents\Hajinmon folder for saves and the shader cache.
    return std::unique_ptr<Doa4App>(new Doa4App(ctx, "Hajinmon",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override { doa4::MigrateUserData(paths); }

  void OnPostInitLogging() override {
    doa4::InstallCrashTrace();
    doa4::ApplyInputDefaults();
  }

  // The runtime answers "ok" for Partition0/Cache0/Cache1 with a NullDevice, so
  // DOA4 believes there is a hard drive and tries to use its cache partition:
  // it formats it with StfsCreateDevice (a stub), links cache: to
  // \Device\cache0 and looks for the .afs archives there. The runtime cannot
  // provide that cache, so we remove the NullDevice: the game sees a console
  // without a hard drive (like a 360 Core) and reads everything from game:\.
  // --renderer=plume replaces the SDK's Xenos emulation with the native renderer.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (!config.graphics) {
      config.graphics = doa4::CreateSelectedGraphicsSystem();
      graphics_ = config.graphics.get();
    }
  }

  void OnPostSetup() override {
    doa4::AttachWindowToRenderer(graphics_, window());
    doa4::StartFrameCapture(runtime());
    doa4::StartSaveTrace();
    doa4::StartCheats();
    if (runtime()->file_system()->UnregisterDevice("\\Device\\Harddisk0")) {
      REXLOG_INFO("doa4: hard drive disabled (NullDevice removed), no disk cache");
    } else {
      REXLOG_ERROR("doa4: NullDevice \\Device\\Harddisk0 not found");
    }
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

 private:
  rex::system::IGraphicsSystem* graphics_ = nullptr;  // Set only for the native renderer.
};
