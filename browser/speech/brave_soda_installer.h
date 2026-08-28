/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_BROWSER_SPEECH_BRAVE_SODA_INSTALLER_H_
#define BRAVE_BROWSER_SPEECH_BRAVE_SODA_INSTALLER_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "brave/components/local_ai/core/on_device_speech_models_state.h"
#include "components/soda/soda_installer.h"

class PrefService;

namespace speech {

// The global `SodaInstaller`, replaced so that installing on-device speech
// recognition installs Brave's own model rather than SODA.
//
// `OnDeviceSpeechRecognitionImpl::Install()` parks its reply, calls
// `InstallSoda` then `InstallLanguage`, and settles the reply from
// `OnSodaInstalled` / `OnSodaInstallError`. Availability is answered elsewhere,
// by `GetBraveOnDeviceSpeechAvailability`.
//
// The base class drives SODA's own install and uninstall lifecycle on component
// ids, directories and language pack prefs that Brave does not have, so those
// overrides no-op and the whole install runs from `InstallLanguage`.
class BraveSodaInstaller
    : public SodaInstaller,
      public local_ai::OnDeviceSpeechModelsState::Observer {
 public:
  BraveSodaInstaller();
  ~BraveSodaInstaller() override;
  BraveSodaInstaller(const BraveSodaInstaller&) = delete;
  BraveSodaInstaller& operator=(const BraveSodaInstaller&) = delete;

  // SodaInstaller:
  void InstallLanguage(std::string_view language,
                       PrefService* global_prefs) override;
  std::vector<std::string> GetLiveCaptionEnabledLanguages() const override;
  std::vector<std::string> GetAvailableLanguages() const override;
  base::FilePath GetSodaBinaryPath() const override;
  base::FilePath GetLanguagePath(std::string_view language) const override;
  void Init(PrefService* profile_prefs, PrefService* global_prefs) override;
  void UninstallLanguage(std::string_view language,
                         PrefService* global_prefs) override;
  void RegisterLanguage(std::string_view language,
                        PrefService* global_prefs) override;
  void UnregisterLanguage(std::string_view language,
                          PrefService* global_prefs) override;

 protected:
  // SodaInstaller:
  void InstallSoda(PrefService* global_prefs) override;
  void UninstallSoda(PrefService* global_prefs) override;

 private:
  // local_ai::OnDeviceSpeechModelsState::Observer:
  void OnSpeechModelDirChanged(const base::FilePath& model_dir) override;
  // Whatever went wrong, upstream has one way to say so. The component
  // installer decides what counts as a failure worth reporting.
  void OnSpeechModelInstallError() override;
};

}  // namespace speech

#endif  // BRAVE_BROWSER_SPEECH_BRAVE_SODA_INSTALLER_H_
