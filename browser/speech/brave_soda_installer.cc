/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/speech/brave_soda_installer.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "brave/components/local_ai/core/on_device_speech_models_component_installer.h"
#include "components/soda/constants.h"

namespace speech {

BraveSodaInstaller::BraveSodaInstaller() {
  local_ai::OnDeviceSpeechModelsState::GetInstance()->AddObserver(this);
}

BraveSodaInstaller::~BraveSodaInstaller() {
  local_ai::OnDeviceSpeechModelsState::GetInstance()->RemoveObserver(this);
}

void BraveSodaInstaller::Init(PrefService* profile_prefs,
                              PrefService* global_prefs) {}

void BraveSodaInstaller::InstallSoda(PrefService* global_prefs) {}

void BraveSodaInstaller::UninstallSoda(PrefService* global_prefs) {}

void BraveSodaInstaller::UninstallLanguage(std::string_view language,
                                           PrefService* global_prefs) {}

void BraveSodaInstaller::RegisterLanguage(std::string_view language,
                                          PrefService* global_prefs) {}

void BraveSodaInstaller::UnregisterLanguage(std::string_view language,
                                            PrefService* global_prefs) {}

void BraveSodaInstaller::InstallLanguage(std::string_view language,
                                         PrefService* global_prefs) {
  auto* state = local_ai::OnDeviceSpeechModelsState::GetInstance();
  if (state->IsModelInstalled()) {
    return;
  }

  local_ai::MaybeRegisterOnDeviceSpeechModelsComponent();
}

std::vector<std::string> BraveSodaInstaller::GetLiveCaptionEnabledLanguages()
    const {
  // Brave's model serves English only. `install()` rejects anything outside
  // this list before it reaches `InstallLanguage`, which is what keeps a
  // request for another language from installing an English model.
  return {GetLanguageName(LanguageCode::kEnUs)};
}

std::vector<std::string> BraveSodaInstaller::GetAvailableLanguages() const {
  return GetLiveCaptionEnabledLanguages();
}

base::FilePath BraveSodaInstaller::GetSodaBinaryPath() const {
  return base::FilePath();
}

base::FilePath BraveSodaInstaller::GetLanguagePath(
    std::string_view language) const {
  return base::FilePath();
}

void BraveSodaInstaller::OnSpeechModelInstallError() {
  NotifyOnSodaInstallError(LanguageCode::kEnUs, ErrorCode::kUnspecifiedError);
}

void BraveSodaInstaller::OnSpeechModelDirChanged(
    const base::FilePath& model_dir) {
  if (model_dir.empty()) {
    soda_binary_installed_ = false;
    installed_languages_.erase(LanguageCode::kEnUs);
    return;
  }

  // Brave's model covers what upstream splits into a binary and a language
  // pack, so both flip together.
  soda_binary_installed_ = true;
  installed_languages_.insert(LanguageCode::kEnUs);
  NotifyOnSodaInstalled(LanguageCode::kEnUs);
}

}  // namespace speech
