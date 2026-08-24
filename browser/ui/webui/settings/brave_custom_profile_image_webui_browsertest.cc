// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <cstdint>
#include <string>
#include <string_view>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/location.h"
#include "base/path_service.h"
#include "base/test/scoped_feature_list.h"
#include "brave/browser/ui/webui/custom_profile_image/features.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "chrome/test/base/ui_test_utils.h"
#include "chrome/test/base/web_ui_mocha_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/ui_base_switches.h"
#include "url/gurl.h"

namespace {

constexpr char kMochaBootstrap[] = R"(
  (async () => {
    await import('chrome://webui-test/mocha.js');
    await import('chrome://webui-test/mocha_adapter_simple.js');
    return true;
  })()
)";

testing::AssertionResult ReadBraveWebUITestFile(
    const base::FilePath& filename,
    std::string* script,
    base::Location location = base::Location::Current()) {
  SCOPED_TRACE(location.ToString());
  const base::FilePath path =
      base::PathService::CheckedGet(base::DIR_SRC_TEST_DATA_ROOT)
          .AppendASCII("brave")
          .AppendASCII("test")
          .AppendASCII("data")
          .AppendASCII("webui")
          .AppendASCII("custom_profile_image")
          .Append(filename);
  if (!base::ReadFileToString(path, script)) {
    return testing::AssertionFailure() << "Could not read " << path;
  }
  return testing::AssertionSuccess();
}

testing::AssertionResult ExecuteBraveWebUITestFile(
    content::WebContents* web_contents,
    const std::string& script,
    int32_t world_id,
    base::Location location = base::Location::Current()) {
  SCOPED_TRACE(location.ToString());
  return content::ExecJs(web_contents, script,
                         content::EXECUTE_SCRIPT_DEFAULT_OPTIONS, world_id);
}

void ExpectMochaSuccess(content::WebContents* web_contents,
                        base::Location location = base::Location::Current()) {
  SCOPED_TRACE(location.ToString());
  auto [success, sub_test_results] =
      webui::ProcessMessagesFromJsTest(web_contents);
  for (const auto& sub_test_result : sub_test_results) {
    if (sub_test_result.failure_reason) {
      ADD_FAILURE() << sub_test_result.name << ": "
                    << *sub_test_result.failure_reason;
    }
  }
  EXPECT_TRUE(success);
}

class BraveCustomProfileImageRowWebUIBrowserTest
    : public WebUIMochaBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    WebUIMochaBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(ReadBraveWebUITestFile(
        base::FilePath::FromASCII("custom_profile_image_row_test.js"),
        &custom_image_row_test_));
  }

  void RunCustomProfileImageRowTest(
      std::string_view trigger,
      base::Location location = base::Location::Current()) {
    SCOPED_TRACE(location.ToString());
    const GURL settings_url("chrome://settings/");
    PrepareWebUITest(settings_url, location);

    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_NE(web_contents, nullptr);
    ASSERT_TRUE(content::ExecJs(web_contents,
                                "registerBraveCustomProfileImageRowTests()",
                                content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                                ISOLATED_WORLD_ID_BRAVE_INTERNAL));
    ASSERT_TRUE(content::ExecJs(web_contents, trigger,
                                content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                                ISOLATED_WORLD_ID_BRAVE_INTERNAL));
    ExpectMochaSuccess(web_contents);
  }

  void PrepareWebUITest(const GURL& settings_url,
                        base::Location location = base::Location::Current()) {
    SCOPED_TRACE(location.ToString());
    ASSERT_TRUE(settings_url.is_valid());
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), settings_url));

    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_NE(web_contents, nullptr);
    ASSERT_TRUE(content::WaitForLoadStop(web_contents));
    ASSERT_EQ(settings_url, web_contents->GetURL());

    ASSERT_TRUE(ExecuteBraveWebUITestFile(web_contents, custom_image_row_test_,
                                          content::ISOLATED_WORLD_ID_GLOBAL));
    ASSERT_TRUE(content::ExecJs(web_contents,
                                "installBraveCustomProfileImageTestDriver()"));

    ASSERT_TRUE(content::ExecJs(web_contents, kMochaBootstrap,
                                content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                                ISOLATED_WORLD_ID_BRAVE_INTERNAL));
    ASSERT_TRUE(ExecuteBraveWebUITestFile(web_contents, custom_image_row_test_,
                                          ISOLATED_WORLD_ID_BRAVE_INTERNAL));
  }

 private:
  std::string custom_image_row_test_;
};

IN_PROC_BROWSER_TEST_F(BraveCustomProfileImageRowWebUIBrowserTest, Behavior) {
  RunCustomProfileImageRowTest("mocha.run()");
}

class BraveCustomProfileImageRowDarkModeWebUIBrowserTest
    : public BraveCustomProfileImageRowWebUIBrowserTest {
 protected:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    WebUIMochaBrowserTest::SetUpCommandLine(command_line);
    command_line->AppendSwitch(switches::kForceDarkMode);
  }
};

IN_PROC_BROWSER_TEST_F(BraveCustomProfileImageRowDarkModeWebUIBrowserTest,
                       SelectedIndicatorUsesDarkForeground) {
  RunCustomProfileImageRowTest(
      "runMochaTest('BraveCustomProfileImageRowTests', "
      "'UsesSelectedIndicatorColorsForCurrentColorScheme')");
  content::WebContents* web_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(web_contents, nullptr);
  ASSERT_TRUE(
      content::EvalJs(web_contents,
                      "matchMedia('(prefers-color-scheme: dark)').matches",
                      content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                      ISOLATED_WORLD_ID_BRAVE_INTERNAL)
          .ExtractBool());
}

class BraveSettingsCustomProfileImageWebUIBrowserTest
    : public BraveCustomProfileImageRowWebUIBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    BraveCustomProfileImageRowWebUIBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(ReadBraveWebUITestFile(
        base::FilePath::FromASCII("settings_manage_profile_test.js"),
        &settings_manage_profile_test_));
  }

  void RunManageProfileTest(
      bool expected_row,
      std::string_view trigger,
      base::Location location = base::Location::Current()) {
    SCOPED_TRACE(location.ToString());
    const GURL manage_profile_url("chrome://settings/manageProfile");
    PrepareWebUITest(manage_profile_url, location);

    content::WebContents* web_contents =
        browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_NE(web_contents, nullptr);
    ASSERT_TRUE(ExecuteBraveWebUITestFile(web_contents,
                                          settings_manage_profile_test_,
                                          ISOLATED_WORLD_ID_BRAVE_INTERNAL));
    ASSERT_TRUE(content::ExecJs(
        web_contents,
        content::JsReplace("registerBraveSettingsManageProfileTests($1)",
                           expected_row),
        content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
        ISOLATED_WORLD_ID_BRAVE_INTERNAL));
    ASSERT_TRUE(content::ExecJs(web_contents, trigger,
                                content::EXECUTE_SCRIPT_DEFAULT_OPTIONS,
                                ISOLATED_WORLD_ID_BRAVE_INTERNAL));
    ExpectMochaSuccess(web_contents);
  }

 private:
  std::string settings_manage_profile_test_;
};

class BraveSettingsCustomProfileImageDefaultWebUIBrowserTest
    : public BraveSettingsCustomProfileImageWebUIBrowserTest {
 public:
  BraveSettingsCustomProfileImageDefaultWebUIBrowserTest() {
    scoped_feature_list_.InitAndDisableFeature(
        custom_profile_image::features::kBraveCustomProfileImage);
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(BraveSettingsCustomProfileImageDefaultWebUIBrowserTest,
                       ManageProfile) {
  RunManageProfileTest(/*expected_row=*/false, "mocha.run()");
}

class BraveSettingsCustomProfileImageEnabledWebUIBrowserTest
    : public BraveSettingsCustomProfileImageWebUIBrowserTest {
 public:
  BraveSettingsCustomProfileImageEnabledWebUIBrowserTest() {
    scoped_feature_list_.InitAndEnableFeature(
        custom_profile_image::features::kBraveCustomProfileImage);
  }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

IN_PROC_BROWSER_TEST_F(BraveSettingsCustomProfileImageEnabledWebUIBrowserTest,
                       ManageProfile) {
  RunManageProfileTest(
      /*expected_row=*/true,
      "runMochaSuite('ManageProfileCustomProfileImageEnabled')");
}

}  // namespace
