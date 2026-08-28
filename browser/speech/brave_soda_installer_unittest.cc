/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/speech/brave_soda_installer.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/values.h"
#include "brave/components/brave_component_updater/browser/mock_on_demand_updater.h"
#include "brave/components/local_ai/core/features.h"
#include "brave/components/local_ai/core/on_device_speech_models_component_installer.h"
#include "brave/components/local_ai/core/on_device_speech_models_state.h"
#include "brave/components/local_ai/core/pref_names.h"
#include "chrome/test/base/testing_browser_process.h"
#include "components/component_updater/component_updater_service.h"
#include "components/component_updater/mock_component_updater_service.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/testing_pref_service.h"
#include "components/soda/constants.h"
#include "components/soda/pref_names.h"
#include "components/update_client/crx_update_item.h"
#include "components/update_client/update_client_errors.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace speech {

namespace {

// Records what the installer reported. Upstream's own observer is
// OnDeviceSpeechRecognitionImpl, which settles the reply parked by
// SpeechRecognition.install() from exactly these notifications.
class TestSodaObserver : public SodaInstaller::Observer {
 public:
  // SodaInstaller::Observer:
  void OnSodaInstalled(LanguageCode language_code) override {
    ++installed_count_;
  }
  void OnSodaInstallError(LanguageCode language_code,
                          SodaInstaller::ErrorCode error_code) override {
    ++error_count_;
  }
  void OnSodaProgress(LanguageCode language_code, int progress) override {
    ++progress_count_;
  }

  int installed_count() const { return installed_count_; }
  int error_count() const { return error_count_; }
  int progress_count() const { return progress_count_; }

 private:
  int installed_count_ = 0;
  int error_count_ = 0;
  int progress_count_ = 0;
};

}  // namespace

class BraveSodaInstallerUnitTest : public testing::Test {
 public:
  void SetUp() override {
    feature_list_.InitAndEnableFeature(
        local_ai::kBraveOnDeviceSpeechRecognition);

    auto cus =
        std::make_unique<component_updater::MockComponentUpdateService>();
    cus_ = cus.get();
    TestingBrowserProcess::GetGlobal()->SetComponentUpdater(std::move(cus));

    // The registrar holds the update service and the local state that
    // `MaybeRegisterOnDeviceSpeechModelsComponent` uses, so it has to be
    // running for `InstallLanguage` to reach the component updater at all.
    // Starting it registers the component, which every test would otherwise
    // see on top of its own call, so settle that here.
    bool registered = false;
    EXPECT_CALL(*cus_, RegisterComponent(testing::_))
        .WillRepeatedly([&registered]() {
          registered = true;
          return true;
        });
    EXPECT_CALL(on_demand_updater_, EnsureInstalled(testing::_, testing::_))
        .Times(testing::AnyNumber());
    // The registrar watches the update service for the whole session. Hold on
    // to what it registered so a test can send events the way the update
    // service itself would.
    EXPECT_CALL(*cus_, AddObserver(testing::_))
        .Times(testing::AnyNumber())
        .WillRepeatedly([this](component_updater::ServiceObserver* observer) {
          cus_observer_ = observer;
        });
    EXPECT_CALL(*cus_, RemoveObserver(testing::_))
        .Times(testing::AnyNumber())
        .WillRepeatedly([this](component_updater::ServiceObserver* observer) {
          cus_observer_ = nullptr;
        });
    local_ai::ManageOnDeviceSpeechModelsComponentRegistration(cus_,
                                                              local_state());
    ASSERT_TRUE(base::test::RunUntil([&registered]() { return registered; }));
    testing::Mock::VerifyAndClearExpectations(cus_);
    testing::Mock::VerifyAndClearExpectations(&on_demand_updater_);
  }

  void TearDown() override {
    if (installer_) {
      installer_->RemoveObserver(&observer_);
      installer_.reset();
    }
    // Drops the registrar's pointers to this test's update service and local
    // state before either goes away.
    local_ai::ShutdownOnDeviceSpeechModelsComponentRegistration();
    TestingBrowserProcess::GetGlobal()->SetComponentUpdater(nullptr);
    local_ai::OnDeviceSpeechModelsState::GetInstance()->SetInstallDir(
        base::FilePath());
  }

 protected:
  // Deferred so a test can publish a model, or arrange the component updater,
  // before the installer observes either.
  void CreateInstaller() {
    installer_ = std::make_unique<BraveSodaInstaller>();
    installer_->AddObserver(&observer_);
  }

  PrefService* local_state() {
    return TestingBrowserProcess::GetGlobal()->GetTestingLocalState();
  }

  void PublishModel() {
    local_ai::OnDeviceSpeechModelsState::GetInstance()->SetInstallDir(
        base::FilePath(FILE_PATH_LITERAL("/brave/speech/models")));
  }

  void InstallLanguage() {
    installer_->InstallLanguage(GetLanguageName(LanguageCode::kEnUs),
                                local_state());
  }

  // Sends what the update service reports about a download, to whoever it
  // registered. That is the component installer, which decides whether it is
  // a failure worth publishing.
  void SendEvent(update_client::ComponentState state, const std::string& id) {
    update_client::CrxUpdateItem item;
    item.state = state;
    item.id = id;
    cus_observer_->OnEvent(item);
  }

  std::string ComponentId() {
    return std::string(local_ai::kOnDeviceSpeechModelsComponentId);
  }

  content::BrowserTaskEnvironment task_environment_;
  brave_component_updater::MockOnDemandUpdater on_demand_updater_;
  raw_ptr<component_updater::MockComponentUpdateService> cus_ = nullptr;
  raw_ptr<component_updater::ServiceObserver> cus_observer_ = nullptr;
  TestSodaObserver observer_;
  std::unique_ptr<BraveSodaInstaller> installer_;
  base::test::ScopedFeatureList feature_list_;
};

// ---------- The language list ----------

// Tests that the only language offered is the one Brave's model transcribes.
TEST_F(BraveSodaInstallerUnitTest, OffersEnglishOnly) {
  CreateInstaller();

  const std::vector<std::string> expected = {
      GetLanguageName(LanguageCode::kEnUs)};
  EXPECT_EQ(expected, installer_->GetLiveCaptionEnabledLanguages());
  // The same list, so that narrowing one narrows every reader of the other.
  EXPECT_EQ(installer_->GetLiveCaptionEnabledLanguages(),
            installer_->GetAvailableLanguages());
}

// Tests that no SODA path is published. Brave ships neither the SODA library
// nor a SODA language pack, and the consumers that should have the model read
// OnDeviceSpeechModelsState directly.
TEST_F(BraveSodaInstallerUnitTest, PublishesNoSodaPaths) {
  CreateInstaller();

  EXPECT_TRUE(installer_->GetSodaBinaryPath().empty());
  EXPECT_TRUE(installer_->GetLanguagePath(GetLanguageName(LanguageCode::kEnUs))
                  .empty());
}

// ---------- What the model arriving and going away mean ----------

// Tests that the model arriving is reported as SODA being installed, which is
// how a parked SpeechRecognition.install() reply is settled as a success.
TEST_F(BraveSodaInstallerUnitTest, ModelArrivingReportsInstalled) {
  CreateInstaller();
  ASSERT_FALSE(installer_->IsSodaInstalled(LanguageCode::kEnUs));

  PublishModel();

  EXPECT_EQ(1, observer_.installed_count());
  EXPECT_TRUE(installer_->IsSodaInstalled(LanguageCode::kEnUs));
}

// Tests that the model going away stops it being reported as installed.
// Without this edge Install() would keep resolving true with no model.
TEST_F(BraveSodaInstallerUnitTest, ModelRemovalClearsInstalled) {
  PublishModel();
  CreateInstaller();
  ASSERT_TRUE(installer_->IsSodaInstalled(LanguageCode::kEnUs));

  local_ai::OnDeviceSpeechModelsState::GetInstance()->SetInstallDir(
      base::FilePath());

  EXPECT_FALSE(installer_->IsSodaInstalled(LanguageCode::kEnUs));
  EXPECT_EQ(0, observer_.installed_count());
}

// Tests that an installer built after the model arrived already reports it,
// since the state replays to a consumer created after ComponentReady.
TEST_F(BraveSodaInstallerUnitTest, ConstructionAdoptsExistingInstall) {
  PublishModel();

  CreateInstaller();

  EXPECT_TRUE(installer_->IsSodaInstalled(LanguageCode::kEnUs));
}

// Tests that a download is never reported as in progress. That state is
// process-global, so reporting it would tell every origin that some origin
// started a download.
TEST_F(BraveSodaInstallerUnitTest, NeverReportsDownloading) {
  CreateInstaller();
  EXPECT_FALSE(installer_->IsSodaLanguageDownloading(LanguageCode::kEnUs));

  bool requested = false;
  EXPECT_CALL(on_demand_updater_, EnsureInstalled(ComponentId(), testing::_))
      .WillRepeatedly([&requested]() { requested = true; });
  EXPECT_CALL(*cus_, RegisterComponent(testing::_))
      .WillRepeatedly(testing::Return(true));
  InstallLanguage();
  ASSERT_TRUE(base::test::RunUntil([&requested]() { return requested; }));

  EXPECT_FALSE(installer_->IsSodaLanguageDownloading(LanguageCode::kEnUs));
  EXPECT_EQ(0, observer_.progress_count());

  PublishModel();
  EXPECT_FALSE(installer_->IsSodaLanguageDownloading(LanguageCode::kEnUs));
}

// ---------- SODA's own lifecycle, which Brave does not run ----------

// Tests that SODA's own language pack bookkeeping is left exactly as upstream
// registered it. RegisterLanguage would record a pack and arm its deletion,
// and UnregisterLanguage would erase the entry the pref is registered with, so
// running either would be visible here. Brave installs no such pack.
TEST_F(BraveSodaInstallerUnitTest, LeavesSodaLanguagePackPrefAlone) {
  CreateInstaller();
  const std::string language = GetLanguageName(LanguageCode::kEnUs);
  const base::ListValue before =
      local_state()->GetList(::prefs::kSodaRegisteredLanguagePacks).Clone();
  // Otherwise upstream's UnregisterLanguage would have nothing to erase and
  // this would pass without asserting anything.
  ASSERT_FALSE(before.empty());

  installer_->RegisterLanguage(language, local_state());
  installer_->UnregisterLanguage(language, local_state());
  installer_->Init(local_state(), local_state());

  EXPECT_EQ(before,
            local_state()->GetList(::prefs::kSodaRegisteredLanguagePacks));
}

// ---------- An install that is already done ----------

// Tests that a model already on disk stops the call there, rather than asking
// for a download of what is already installed. Nothing is reported, because
// nothing changed.
TEST_F(BraveSodaInstallerUnitTest, InstallLanguageWithModelDoesNothing) {
  PublishModel();
  CreateInstaller();
  ASSERT_TRUE(installer_->IsSodaInstalled(LanguageCode::kEnUs));

  EXPECT_CALL(*cus_, RegisterComponent(testing::_)).Times(0);
  EXPECT_CALL(on_demand_updater_, EnsureInstalled(ComponentId(), testing::_))
      .Times(0);

  InstallLanguage();

  EXPECT_EQ(0, observer_.installed_count());
  EXPECT_EQ(0, observer_.error_count());
}

// ---------- Settling a parked reply ----------

// Tests that a request that cannot be acted on is reported as a failure rather
// than left to hang. The reply is parked before InstallLanguage is called, so
// every path out of it has to report.
TEST_F(BraveSodaInstallerUnitTest, InstallLanguageWhenSwitchedOffReportsError) {
  CreateInstaller();
  base::test::ScopedFeatureList disabled;
  disabled.InitAndDisableFeature(local_ai::kBraveOnDeviceSpeechRecognition);

  InstallLanguage();
  EXPECT_EQ(0, observer_.error_count());

  ASSERT_TRUE(
      base::test::RunUntil([this]() { return observer_.error_count() == 1; }));
  EXPECT_EQ(0, observer_.installed_count());
}

// Tests that a failure reaches upstream even when this installer did not ask
// for the install. The component is registered at startup, so an install can
// fail with no reply parked, and reporting it anyway costs nothing because
// upstream finds nothing waiting on it.
TEST_F(BraveSodaInstallerUnitTest, ReportsInstallErrorNobodyAskedFor) {
  CreateInstaller();

  local_ai::OnDeviceSpeechModelsState::GetInstance()->NotifyInstallError();

  EXPECT_EQ(1, observer_.error_count());
  EXPECT_EQ(0, observer_.installed_count());
}

// ---------- Watching the component updater ----------

// Tests a download already in flight, which this request only joins. Nothing
// about how that download ends comes back from the request, so the failure
// reaches upstream by way of the update service telling the component
// installer, and exactly once.
TEST_F(BraveSodaInstallerUnitTest, InProgressDownloadIsSettledByItsFailure) {
  CreateInstaller();
  EXPECT_CALL(*cus_, RegisterComponent(testing::_))
      .WillRepeatedly(testing::Return(true));
  bool answered = false;
  EXPECT_CALL(on_demand_updater_, EnsureInstalled(ComponentId(), testing::_))
      .WillOnce([&answered](const std::string& id,
                            component_updater::Callback callback) {
        std::move(callback).Run(update_client::Error::UPDATE_IN_PROGRESS);
        answered = true;
      });

  InstallLanguage();
  ASSERT_TRUE(base::test::RunUntil([&answered]() { return answered; }));

  // Joining a download in progress is not itself a failure.
  EXPECT_EQ(0, observer_.error_count());

  SendEvent(update_client::ComponentState::kUpdateError, ComponentId());

  // Published from a task, so that a caller settling a parked reply is never
  // re-entered while it is still setting the install up.
  EXPECT_TRUE(
      base::test::RunUntil([this]() { return observer_.error_count() == 1; }));
}

// Tests the same download succeeding instead. The install dir being published
// is what reports it, so nothing is expected from the component updater.
TEST_F(BraveSodaInstallerUnitTest, InProgressDownloadIsSettledByItsSuccess) {
  CreateInstaller();
  EXPECT_CALL(*cus_, RegisterComponent(testing::_))
      .WillRepeatedly(testing::Return(true));
  bool answered = false;
  EXPECT_CALL(on_demand_updater_, EnsureInstalled(ComponentId(), testing::_))
      .WillOnce([&answered](const std::string& id,
                            component_updater::Callback callback) {
        std::move(callback).Run(update_client::Error::UPDATE_IN_PROGRESS);
        answered = true;
      });

  InstallLanguage();
  ASSERT_TRUE(base::test::RunUntil([&answered]() { return answered; }));
  ASSERT_EQ(0, observer_.error_count());

  PublishModel();

  EXPECT_EQ(1, observer_.installed_count());
  EXPECT_EQ(0, observer_.error_count());
}

}  // namespace speech
