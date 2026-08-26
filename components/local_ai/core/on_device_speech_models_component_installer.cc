/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/local_ai/core/on_device_speech_models_component_installer.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/task/bind_post_task.h"
#include "base/values.h"
#include "base/version.h"
#include "brave/components/brave_component_updater/browser/brave_on_demand_updater.h"
#include "brave/components/local_ai/core/features.h"
#include "brave/components/local_ai/core/on_device_speech_models_state.h"
#include "brave/components/local_ai/core/pref_names.h"
#include "components/component_updater/component_installer.h"
#include "components/component_updater/component_updater_service.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_service.h"
#include "components/update_client/update_client.h"
#include "components/update_client/update_client_errors.h"
#include "crypto/sha2.h"

namespace local_ai {

namespace {

constexpr base::FilePath::CharType kComponentInstallDir[] =
    FILE_PATH_LITERAL("BraveOnDeviceSpeechModels");
constexpr char kComponentName[] = "Brave On-Device Speech Models";

// SHA256 of the provisioned component's public key.
constexpr uint8_t kPublicKeySHA256[32] = {
    0xd7, 0xa4, 0xa2, 0x24, 0x53, 0xff, 0xef, 0x1b, 0x3e, 0xa8, 0x1a,
    0xe4, 0x6f, 0xf0, 0xd1, 0x10, 0xac, 0xaa, 0x39, 0x3b, 0x03, 0xcd,
    0xf1, 0x10, 0x04, 0x5e, 0xf9, 0x33, 0xf5, 0xe9, 0x6c, 0x4d};
static_assert(std::size(kPublicKeySHA256) == crypto::kSHA256Length,
              "Wrong hash length");

// Whether the component may be installed. The feature is fixed for the session
// but `kBraveLocalAIEnabled` is managed by Brave Origin and can flip at any
// time.
bool IsComponentAllowed(const PrefService* local_state) {
  return local_state &&
         base::FeatureList::IsEnabled(kBraveOnDeviceSpeechRecognition) &&
         local_state->GetBoolean(prefs::kBraveLocalAIEnabled);
}

// Owns the component registration for the whole session and watches the master
// switch so it stays in sync with it. Registering and removing the model both
// go through the one `ComponentInstaller` this holds, which is what orders a
// removal against an install rather than racing it on an unrelated sequence.
class OnDeviceSpeechModelsComponentRegistrar {
 public:
  static OnDeviceSpeechModelsComponentRegistrar* GetInstance() {
    static base::NoDestructor<OnDeviceSpeechModelsComponentRegistrar> instance;
    return instance.get();
  }

  OnDeviceSpeechModelsComponentRegistrar(
      const OnDeviceSpeechModelsComponentRegistrar&) = delete;
  OnDeviceSpeechModelsComponentRegistrar& operator=(
      const OnDeviceSpeechModelsComponentRegistrar&) = delete;

  // Once per process, or once per `Shutdown`.
  void Start(component_updater::ComponentUpdateService* cus,
             PrefService* local_state) {
    CHECK(pref_change_registrar_.IsEmpty() && !cus_);
    CHECK(local_state);
    started_ = true;
    cus_ = cus;
    pref_change_registrar_.Init(local_state);
    pref_change_registrar_.Add(
        prefs::kBraveLocalAIEnabled,
        base::BindRepeating(&OnDeviceSpeechModelsComponentRegistrar::Sync,
                            base::Unretained(this)));
    Sync();
  }

  void Shutdown() {
    started_ = false;
    pref_change_registrar_.Reset();
    cus_ = nullptr;
    installer_.reset();
    registration_pending_ = false;
    RunPendingCallbacks(update_client::Error::INVALID_ARGUMENT);
  }

  // Registers the component, which also requests the download. Joins a
  // registration already in flight instead of starting a second one.
  void Register(component_updater::Callback callback) {
    // Keep delivery consistently asynchronous across all branches below.
    callback = base::BindPostTaskToCurrentDefault(std::move(callback));

    if (!cus_ || !IsComponentAllowed(pref_change_registrar_.prefs())) {
      std::move(callback).Run(update_client::Error::INVALID_ARGUMENT);
      return;
    }

    pending_callbacks_.push_back(std::move(callback));
    if (registration_pending_) {
      return;
    }
    registration_pending_ = true;
    EnsureInstaller();
    installer_->Register(
        cus_,
        base::BindOnce(&OnDeviceSpeechModelsComponentRegistrar::OnRegistered,
                       base::Unretained(this)));
  }

 private:
  friend base::NoDestructor<OnDeviceSpeechModelsComponentRegistrar>;

  OnDeviceSpeechModelsComponentRegistrar() = default;
  ~OnDeviceSpeechModelsComponentRegistrar() = default;

  void Sync() {
    if (!IsComponentAllowed(pref_change_registrar_.prefs())) {
      Unregister();
      return;
    }
    Register(base::DoNothing());
  }

  void OnRegistered() {
    registration_pending_ = false;
    // `Shutdown` ran while this was in flight. Falling through would read the
    // prefs it dropped as the master switch being off and remove the model.
    if (!started_) {
      RunPendingCallbacks(update_client::Error::INVALID_ARGUMENT);
      return;
    }
    // The switch turned off while this was in flight, so the unregister it ran
    // found nothing and left the files alone. The component reaches the update
    // service only now, and no further pref change is coming to try again, so
    // finish the removal here.
    if (!IsComponentAllowed(pref_change_registrar_.prefs())) {
      Unregister();
      RunPendingCallbacks(update_client::Error::INVALID_ARGUMENT);
      return;
    }
    // Registering has already published whatever was on disk, so a model
    // already downloaded stays usable. Only the download is refused, which is
    // what --disable-component-update forbids and what trips a DCHECK in
    // BraveOnDemandUpdater.
    if (brave_component_updater::BraveOnDemandUpdater::GetInstance()
            ->is_component_update_disabled()) {
      RunPendingCallbacks(update_client::Error::INVALID_ARGUMENT);
      return;
    }
    brave_component_updater::BraveOnDemandUpdater::GetInstance()
        ->EnsureInstalled(
            kOnDeviceSpeechModelsComponentId,
            base::BindOnce(
                &OnDeviceSpeechModelsComponentRegistrar::RunPendingCallbacks,
                base::Unretained(this)));
  }

  void Unregister() {
    // Only remove the files ourselves when the service did not (it uninstalls
    // what it had registered) and no registration in flight may still install
    // them.
    const bool was_registered =
        cus_ && cus_->UnregisterComponent(kOnDeviceSpeechModelsComponentId);
    // Published before the files go, so nothing acts on a model whose files
    // are on their way out.
    OnDeviceSpeechModelsState::GetInstance()->SetInstallDir(base::FilePath());
    if (!was_registered && !registration_pending_) {
      EnsureInstaller();
      installer_->Uninstall();
    }
  }

  void RunPendingCallbacks(update_client::Error error) {
    std::vector<component_updater::Callback> callbacks;
    callbacks.swap(pending_callbacks_);
    for (auto& callback : callbacks) {
      std::move(callback).Run(error);
    }
  }

  void EnsureInstaller() {
    if (!installer_) {
      installer_ = base::MakeRefCounted<component_updater::ComponentInstaller>(
          std::make_unique<OnDeviceSpeechModelsComponentInstallerPolicy>(
              pref_change_registrar_.prefs()));
    }
  }

  bool started_ = false;
  raw_ptr<component_updater::ComponentUpdateService> cus_ = nullptr;
  PrefChangeRegistrar pref_change_registrar_;
  // True from `Register` until `OnRegistered`. The component is absent from
  // the update service for that whole window, so a second `Register` would
  // register it twice and an `Unregister` would find nothing to unregister.
  bool registration_pending_ = false;
  // Answered together, because everyone who asked while one registration was
  // in flight is waiting on that same registration.
  std::vector<component_updater::Callback> pending_callbacks_;
  // Reused: registration and uninstall share the installer's task runner, so a
  // per-registration instance would let an uninstall delete what the next
  // registration installed.
  scoped_refptr<component_updater::ComponentInstaller> installer_;
};

}  // namespace

OnDeviceSpeechModelsComponentInstallerPolicy::
    OnDeviceSpeechModelsComponentInstallerPolicy(PrefService* local_state)
    : local_state_(local_state) {}
OnDeviceSpeechModelsComponentInstallerPolicy::
    ~OnDeviceSpeechModelsComponentInstallerPolicy() = default;

bool OnDeviceSpeechModelsComponentInstallerPolicy::VerifyInstallation(
    const base::DictValue& manifest,
    const base::FilePath& install_dir) const {
  return base::DirectoryExists(install_dir.AppendASCII(kModelDirName));
}

bool OnDeviceSpeechModelsComponentInstallerPolicy::
    SupportsGroupPolicyEnabledComponentUpdates() const {
  return false;
}

bool OnDeviceSpeechModelsComponentInstallerPolicy::RequiresNetworkEncryption()
    const {
  return false;
}

update_client::CrxInstaller::Result
OnDeviceSpeechModelsComponentInstallerPolicy::OnCustomInstall(
    const base::DictValue& manifest,
    const base::FilePath& install_dir) {
  return update_client::CrxInstaller::Result(update_client::InstallError::NONE);
}

void OnDeviceSpeechModelsComponentInstallerPolicy::OnCustomUninstall() {}

void OnDeviceSpeechModelsComponentInstallerPolicy::ComponentReady(
    const base::Version& version,
    const base::FilePath& install_dir,
    base::DictValue manifest) {
  if (install_dir.empty()) {
    return;
  }
  // Unregistration is deferred behind an in-flight update, so this still fires
  // for a download that started before the switch turned off.
  if (!IsComponentAllowed(local_state_)) {
    return;
  }
  OnDeviceSpeechModelsState::GetInstance()->SetInstallDir(install_dir);
}

base::FilePath
OnDeviceSpeechModelsComponentInstallerPolicy::GetRelativeInstallDir() const {
  return base::FilePath(kComponentInstallDir);
}

void OnDeviceSpeechModelsComponentInstallerPolicy::GetHash(
    std::vector<uint8_t>* hash) const {
  hash->assign(std::begin(kPublicKeySHA256), std::end(kPublicKeySHA256));
}

std::string OnDeviceSpeechModelsComponentInstallerPolicy::GetName() const {
  return kComponentName;
}

update_client::InstallerAttributes
OnDeviceSpeechModelsComponentInstallerPolicy::GetInstallerAttributes() const {
  return update_client::InstallerAttributes();
}

bool OnDeviceSpeechModelsComponentInstallerPolicy::IsBraveComponent() const {
  return true;
}

void ManageOnDeviceSpeechModelsComponentRegistration(
    component_updater::ComponentUpdateService* cus,
    PrefService* local_state) {
  OnDeviceSpeechModelsComponentRegistrar::GetInstance()->Start(cus,
                                                               local_state);
}

void ShutdownOnDeviceSpeechModelsComponentRegistration() {
  OnDeviceSpeechModelsComponentRegistrar::GetInstance()->Shutdown();
}

void MaybeRegisterOnDeviceSpeechModelsComponent(
    component_updater::Callback callback) {
  OnDeviceSpeechModelsComponentRegistrar::GetInstance()->Register(
      std::move(callback));
}

}  // namespace local_ai
