/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/local_ai/core/on_device_speech_models_component_installer.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/task/sequenced_task_runner.h"
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
#include "components/update_client/crx_update_item.h"
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
  return base::FeatureList::IsEnabled(kBraveOnDeviceSpeechRecognition) &&
         (!local_state || local_state->GetBoolean(prefs::kBraveLocalAIEnabled));
}

// Owns the component registration for the whole session and follows the master
// switch so it stays in sync with it. It is also the one place that decides
// what counts as an install failing, weighing what registering answered
// against what the update service reports, and publishes that to
// `OnDeviceSpeechModelsState`. A success reaches the same observers from the
// policy instead, as the install directory `ComponentReady` hands over.
//
// Registering and removing the model both go through the one
// `ComponentInstaller` this holds, which is what orders a removal against an
// install rather than racing it on an unrelated sequence.
class OnDeviceSpeechModelsComponentRegistrar
    : public component_updater::ServiceObserver {
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
    cus_ = cus;
    pref_change_registrar_.Init(local_state);
    pref_change_registrar_.Add(
        prefs::kBraveLocalAIEnabled,
        base::BindRepeating(&OnDeviceSpeechModelsComponentRegistrar::Sync,
                            base::Unretained(this)));
    Sync();
  }

  void Shutdown() {
    pref_change_registrar_.Reset();
    cus_observation_.Reset();
    cus_ = nullptr;
    installer_.reset();
    registration_pending_ = false;
  }

  // Registers the component, which also requests the download. Joins a
  // registration already in flight instead of starting a second one.
  void Register() {
    if (!cus_ || !IsComponentAllowed(pref_change_registrar_.prefs())) {
      ReportError();
      return;
    }

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
  ~OnDeviceSpeechModelsComponentRegistrar() override = default;

  void Sync() {
    if (!IsComponentAllowed(pref_change_registrar_.prefs())) {
      Unregister();
      return;
    }
    Register();
  }

  void OnRegistered() {
    registration_pending_ = false;
    // `Shutdown` ran while this was in flight, so there is no update service
    // left to finish against. Falling through would read the prefs it dropped
    // as the master switch being off and remove the model.
    if (!cus_) {
      return;
    }
    // The switch turned off while this was in flight, so the unregister it ran
    // found nothing and left the files alone. The component reaches the update
    // service only now, and no further pref change is coming to try again, so
    // finish the removal here.
    if (!IsComponentAllowed(pref_change_registrar_.prefs())) {
      Unregister();
      ReportError();
      return;
    }
    // Registering has already published whatever was on disk, so a model
    // already downloaded stays usable. Only the download is refused, which is
    // what --disable-component-update forbids and what trips a DCHECK in
    // BraveOnDemandUpdater.
    if (brave_component_updater::BraveOnDemandUpdater::GetInstance()
            ->is_component_update_disabled()) {
      ReportError();
      return;
    }
    // Watched from the first download we ask for, because that is how its
    // outcome arrives. Every path above returns without asking for one.
    if (!cus_observation_.IsObserving()) {
      cus_observation_.Observe(cus_);
    }
    brave_component_updater::BraveOnDemandUpdater::GetInstance()
        ->EnsureInstalled(
            kOnDeviceSpeechModelsComponentId,
            base::BindOnce(
                &OnDeviceSpeechModelsComponentRegistrar::OnEnsureInstalled,
                base::Unretained(this)));
  }

  void OnEnsureInstalled(update_client::Error error) {
    if (OnDeviceSpeechModelsState::GetInstance()->IsModelInstalled()) {
      return;
    }

    if (error != update_client::Error::NONE &&
        error != update_client::Error::UPDATE_IN_PROGRESS) {
      ReportError();
    }

    // If NONE or UPDATE_IN_PROGRESS, we wait for OnEvent (kUpdated, kUpToDate,
    // or kUpdateError) terminal state to check if model is installed to report
    // error or not.
  }

  // component_updater::ServiceObserver:
  void OnEvent(const update_client::CrxUpdateItem& item) override {
    if (item.id != kOnDeviceSpeechModelsComponentId) {
      return;
    }

    // Reports an error when the update reached a terminal state and the model
    // is still not installed, whether it errored, landed without publishing,
    // or the server had nothing to offer.
    if (!OnDeviceSpeechModelsState::GetInstance()->IsModelInstalled() &&
        (item.state == update_client::ComponentState::kUpdated ||
         item.state == update_client::ComponentState::kUpToDate ||
         item.state == update_client::ComponentState::kUpdateError)) {
      ReportError();
    }
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

  // Posted rather than published inline, so that an observer never runs while
  // whoever asked for the install is still inside the call, part way through
  // setting that install up.
  void ReportError() {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce([] {
          OnDeviceSpeechModelsState::GetInstance()->NotifyInstallError();
        }));
  }

  void EnsureInstaller() {
    if (!installer_) {
      installer_ = base::MakeRefCounted<component_updater::ComponentInstaller>(
          std::make_unique<OnDeviceSpeechModelsComponentInstallerPolicy>(
              pref_change_registrar_.prefs()));
    }
  }

  // Held from `Start` until `Shutdown`, which is also what tells an in-flight
  // registration that it landed too late to be finished.
  raw_ptr<component_updater::ComponentUpdateService> cus_ = nullptr;
  base::ScopedObservation<component_updater::ComponentUpdateService,
                          component_updater::ServiceObserver>
      cus_observation_{this};
  PrefChangeRegistrar pref_change_registrar_;
  // True from `Register` until `OnRegistered`. The component is absent from
  // the update service for that whole window, so a second `Register` would
  // register it twice and an `Unregister` would find nothing to unregister.
  bool registration_pending_ = false;
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

void MaybeRegisterOnDeviceSpeechModelsComponent() {
  OnDeviceSpeechModelsComponentRegistrar::GetInstance()->Register();
}

}  // namespace local_ai
