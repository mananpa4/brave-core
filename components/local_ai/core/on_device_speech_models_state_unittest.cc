/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/local_ai/core/on_device_speech_models_state.h"

#include <vector>

#include "base/files/file_path.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace local_ai {

namespace {

// Records every directory published, so a test can tell "notified with no
// model" from "not notified at all", and one version from the next.
class TestObserver : public OnDeviceSpeechModelsState::Observer {
 public:
  TestObserver() = default;
  ~TestObserver() override = default;

  // OnDeviceSpeechModelsState::Observer:
  void OnSpeechModelDirChanged(const base::FilePath& model_dir) override {
    calls_.push_back(model_dir);
  }
  void OnSpeechModelInstallError() override { ++error_count_; }

  const std::vector<base::FilePath>& calls() const { return calls_; }
  int error_count() const { return error_count_; }

 private:
  std::vector<base::FilePath> calls_;
  int error_count_ = 0;
};

}  // namespace

class OnDeviceSpeechModelsStateUnitTest : public testing::Test {
 public:
  void TearDown() override {
    // The state is a process-wide singleton, so unhook and clear it rather
    // than leaking either into the next test in this binary.
    state()->RemoveObserver(&observer_);
    state()->SetInstallDir(base::FilePath());
  }

 protected:
  OnDeviceSpeechModelsState* state() {
    return OnDeviceSpeechModelsState::GetInstance();
  }

  // No test here touches the filesystem, so this never has to exist.
  const base::FilePath install_dir_{FILE_PATH_LITERAL("/brave/speech/models")};
  TestObserver observer_;
};

// Tests that the model dir and what `IsModelInstalled` reports both follow the
// install dir, in and out.
TEST_F(OnDeviceSpeechModelsStateUnitTest, IsModelInstalledFollowsInstallDir) {
  EXPECT_FALSE(state()->IsModelInstalled());
  EXPECT_TRUE(state()->GetModelDir().empty());

  state()->SetInstallDir(install_dir_);
  EXPECT_TRUE(state()->IsModelInstalled());
  EXPECT_EQ(install_dir_, state()->GetInstallDir());
  EXPECT_EQ(install_dir_.AppendASCII(kModelDirName), state()->GetModelDir());

  state()->SetInstallDir(base::FilePath());
  EXPECT_FALSE(state()->IsModelInstalled());
  EXPECT_TRUE(state()->GetInstallDir().empty());
  EXPECT_TRUE(state()->GetModelDir().empty());
}

// Tests that a consumer can follow the model by observing alone, without
// asking. Every move matters: an update lands on a new version directory and
// deletes the old one, and removal takes the model away entirely, so a
// consumer holding anything derived from the last directory has to redo it.
TEST_F(OnDeviceSpeechModelsStateUnitTest, ObservesEveryModelDirChange) {
  const base::FilePath v1{FILE_PATH_LITERAL("/brave/speech/models/1.0")};
  const base::FilePath v2{FILE_PATH_LITERAL("/brave/speech/models/2.0")};
  state()->AddObserver(&observer_);

  state()->SetInstallDir(v1);

  // A consumer created after the component arrived hears about it too, rather
  // than reporting that it is waiting for something already here.
  TestObserver late_observer;
  state()->AddObserver(&late_observer);
  EXPECT_EQ(std::vector<base::FilePath>({v1.AppendASCII(kModelDirName)}),
            late_observer.calls());
  state()->RemoveObserver(&late_observer);

  // An update landing on the same directory changed nothing.
  state()->SetInstallDir(v1);

  state()->SetInstallDir(v2);
  EXPECT_EQ(v2.AppendASCII(kModelDirName), state()->GetModelDir());

  state()->SetInstallDir(base::FilePath());

  // Subscribing with nothing installed, the install, the update, the removal.
  EXPECT_EQ(std::vector<base::FilePath>(
                {base::FilePath(), v1.AppendASCII(kModelDirName),
                 v2.AppendASCII(kModelDirName), base::FilePath()}),
            observer_.calls());
}

// Tests that a failed install reaches observers. It is the one thing the state
// reports that is not a directory change.
TEST_F(OnDeviceSpeechModelsStateUnitTest, NotifiesInstallError) {
  state()->AddObserver(&observer_);

  state()->NotifyInstallError();

  EXPECT_EQ(1, observer_.error_count());
}

}  // namespace local_ai
