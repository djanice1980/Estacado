#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main() {
  const std::string source_root = REXGLUE_SOURCE_ROOT;
  std::ifstream source_file(
      source_root + "/src/input/mnk/mnk_input_driver.cpp",
      std::ios::binary);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  std::ifstream header_file(
      source_root + "/include/rex/input/mnk/mnk_input_driver.h",
      std::ios::binary);
  const std::string header((std::istreambuf_iterator<char>(header_file)),
                           std::istreambuf_iterator<char>());
  if (source.empty() || header.empty()) {
    std::cerr << "keyboard/mouse driver source was not readable\n";
    return 1;
  }

  const size_t function = source.find("void MnkInputDriver::OnWindowAvailable(");
  const size_t attach = source.find("attached_window_ = window;", function);
  const size_t adopt_focus =
      source.find("has_focus_ = window->HasFocus();", attach);
  const size_t add_input = source.find("window->AddInputListener", adopt_focus);
  const bool neutral_default =
      header.find("bool has_focus_ = false;") != std::string::npos;
  const size_t enable_cvar =
      source.find("REXCVAR_DEFINE_BOOL(input_keyboard_mouse,");
  const size_t enable_restart = source.find(
      ".lifecycle(rex::cvar::Lifecycle::kRequiresRestart);", enable_cvar);
  const size_t user_cvar = source.find(
      "REXCVAR_DEFINE_INT32(input_keyboard_mouse_user_index,", enable_cvar);
  const size_t user_restart = source.find(
      ".lifecycle(rex::cvar::Lifecycle::kRequiresRestart);", user_cvar);
  const size_t sensitivity_cvar =
      source.find("REXCVAR_DEFINE_DOUBLE(input_mouse_sensitivity,", user_cvar);
  const size_t acceleration_cvar =
      source.find("REXCVAR_DEFINE_DOUBLE(input_mouse_acceleration,",
                  sensitivity_cvar);
  const size_t smoothing_cvar =
      source.find("REXCVAR_DEFINE_DOUBLE(input_mouse_smoothing,",
                  acceleration_cvar);
  const bool restart_contract =
      enable_cvar != std::string::npos && enable_restart != std::string::npos &&
      user_cvar != std::string::npos && user_restart != std::string::npos &&
      sensitivity_cvar != std::string::npos &&
      acceleration_cvar != std::string::npos &&
      smoothing_cvar != std::string::npos &&
      enable_cvar < enable_restart &&
      enable_restart < user_cvar && user_cvar < user_restart &&
      user_restart < sensitivity_cvar && sensitivity_cvar < acceleration_cvar &&
      acceleration_cvar < smoothing_cvar;
  const bool passed = function != std::string::npos &&
                      attach != std::string::npos &&
                      adopt_focus != std::string::npos &&
                      add_input != std::string::npos &&
                      function < attach && attach < adopt_focus &&
                      adopt_focus < add_input && neutral_default &&
                      restart_contract;
  if (!passed) {
    std::cerr << "keyboard/mouse startup focus or restart ownership regressed\n";
    return 1;
  }

  std::cout << "Keyboard/mouse focus attachment policy passed\n";
  return 0;
}
