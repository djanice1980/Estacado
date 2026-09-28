#include "runtime_single_instance.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
bool Check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}
bool ChildCanAcquire(const std::wstring& name, DWORD expected) {
  wchar_t executable[32768]{};
  if (!GetModuleFileNameW(nullptr, executable, 32768)) return false;
  std::wstring command = L"\"" + std::wstring(executable) + L"\" --probe-lock " + name;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return false;
  const DWORD waited = WaitForSingleObject(child.hProcess, 10000);
  DWORD result = 99;
  if (waited == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
  else { TerminateProcess(child.hProcess, 99); WaitForSingleObject(child.hProcess, 10000); }
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return waited == WAIT_OBJECT_0 && result == expected;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--probe-lock") {
    const std::string argument = argv[2];
    RuntimeSingleInstance probe(std::wstring(argument.begin(), argument.end()));
    return probe.acquired() ? 3 : 0;
  }
  const std::wstring name =
      L"Local\\TheDarknessRecompiled-SingleInstanceTest-" +
      std::to_wstring(GetCurrentProcessId());
  bool passed = true;
  passed &= Check(!RuntimeSingleInstance::Exists(name), "read-only query created a title lock");
  {
    RuntimeSingleInstance first(name);
    passed &= Check(first.acquired() && !first.alreadyRunning(),
                    "first runtime instance did not acquire the title lock");
    RuntimeSingleInstance second(name);
    passed &= Check(!second.acquired() && second.alreadyRunning(),
                    "competing runtime instance was not rejected");
    passed &= Check(RuntimeSingleInstance::Exists(name), "running title was not detected");
  }
  passed &= Check(!RuntimeSingleInstance::Exists(name), "query retained title ownership after close");
  RuntimeSingleInstance afterRelease(name);
  passed &= Check(afterRelease.acquired() && !afterRelease.alreadyRunning(),
                  "title lock was not released with the owning instance");
  const auto config = std::filesystem::temp_directory_path() /
      (L"darkness-config-lock-" + std::to_wstring(GetCurrentProcessId())) / L"TheDarkness.pc.toml";
  const auto configName = RuntimeConfigLockName(config);
  passed &= Check(configName == RuntimeConfigLockName(config.parent_path() / L"." / L"THEDARKNESS.PC.TOML"),
                  "case/normalized aliases must share configuration ownership");
  {
    RuntimeSingleInstance executingConfig(configName);
    RuntimeSingleInstance sameConfig(configName);
    RuntimeSingleInstance unrelatedConfig(RuntimeConfigLockName(config.parent_path() / L"other.toml"));
    passed &= Check(executingConfig.acquired() && !sameConfig.acquired() && unrelatedConfig.acquired(),
                    "active configuration must be protected without blocking unrelated settings");
    passed &= Check(ChildCanAcquire(configName, 0), "another process bypassed active configuration ownership");
    passed &= Check(ChildCanAcquire(RuntimeConfigLockName(config.parent_path() / L"independent.toml"), 3),
                    "another process could not edit an independent configuration");
    executingConfig.Release();
    executingConfig.Release();
    passed &= Check(!executingConfig.acquired() && ChildCanAcquire(configName, 3),
                    "explicit startup release must allow later writers and be idempotent");
    passed &= Check(afterRelease.acquired() && ChildCanAcquire(name, 0),
                    "releasing config must not release the executing title lock");
  }
  passed &= Check(ChildCanAcquire(configName, 3), "another process could not acquire released configuration");
  RuntimeSingleInstance editableAfterClose(configName);
  passed &= Check(editableAfterClose.acquired(), "configuration must become editable after owner exits");

  const std::string mainPath =
      std::string(DARKNESS_SOURCE_ROOT) + "/runtime/main.cpp";
  std::ifstream mainFile(mainPath, std::ios::binary);
  const std::string mainSource((std::istreambuf_iterator<char>(mainFile)),
                               std::istreambuf_iterator<char>());
  const size_t verifyAction = mainSource.find(
      "launchOptions.action == RuntimeLaunchAction::VerifyPackage");
  const size_t configAction = mainSource.find(
      "launchOptions.action == RuntimeLaunchAction::ValidateConfig");
  const size_t inspectAction = mainSource.find(
      "launchOptions.action == RuntimeLaunchAction::InspectConfig");
  const size_t modsAction = mainSource.find(
      "launchOptions.action == RuntimeLaunchAction::ValidateMods");
  const size_t instanceLock = mainSource.find(
      "RuntimeSingleInstance titleInstance");
  const size_t configLock = mainSource.find("RuntimeSingleInstance configInstance");
  const size_t installAction = mainSource.find(
      "if (launchOptions.action == RuntimeLaunchAction::InstallPreset)");
  const size_t hostStart = mainSource.find("std::cout << \"HOST_START");
  const size_t configRelease = mainSource.find("configInstance.Release()");
  const size_t ownedGraphics = mainSource.find("ConfigureRuntimeGraphicsPcConfig(\n            startupPcConfig.present()");
  const size_t ownedLanguage = mainSource.find("GetGuestXamState().ConfigureLanguage(xboxLanguage)");
  passed &= Check(verifyAction != std::string::npos &&
                      configAction != std::string::npos &&
                      inspectAction != std::string::npos &&
                      modsAction != std::string::npos &&
                      instanceLock != std::string::npos &&
                      installAction != std::string::npos &&
                      configLock != std::string::npos &&
                      hostStart != std::string::npos &&
                      verifyAction < instanceLock && configAction < instanceLock &&
                      inspectAction < instanceLock &&
                      modsAction < instanceLock &&
                      inspectAction < configLock && configLock < installAction &&
                      installAction < instanceLock && instanceLock < hostStart,
                  "config lock must serialize installation; title lock must guard only guest startup");
  passed &= Check(configRelease != std::string::npos &&
                      ownedGraphics != std::string::npos && ownedLanguage != std::string::npos &&
                      ownedGraphics < configRelease && ownedLanguage < configRelease &&
                      instanceLock < configRelease && configRelease < hostStart &&
                      mainSource.find("titleInstance.Release()") == std::string::npos,
                  "config release requires owned CPU/GPU startup while title stays protected");
  if (passed) std::cout << "Runtime single-instance regression passed\n";
  return passed ? 0 : 1;
}
