/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/browser/ui/webui/custom_profile_image_strings/custom_profile_image_strings.h"

#include "base/feature_list.h"
#include "brave/browser/ui/webui/custom_profile_image/features.h"
#include "brave/grit/brave_generated_resources.h"
#include "content/public/browser/web_ui_data_source.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/webui/web_ui_util.h"

namespace brave {

namespace {

constexpr webui::LocalizedString kCustomProfileImageStrings[] = {
    {"customProfileImageUpload", IDS_CUSTOM_PROFILE_IMAGE_UPLOAD_ACTION},
    {"customProfileImageReplace", IDS_CUSTOM_PROFILE_IMAGE_REPLACE_ACTION},
    {"customProfileImageRemove", IDS_CUSTOM_PROFILE_IMAGE_REMOVE_ACTION},
    {"customProfileImagePreviewLabel", IDS_CUSTOM_PROFILE_IMAGE_PREVIEW_LABEL},
    {"customProfileImageUploadTooltip",
     IDS_CUSTOM_PROFILE_IMAGE_UPLOAD_TOOLTIP},
    {"customProfileImageRemoveTooltip",
     IDS_CUSTOM_PROFILE_IMAGE_REMOVE_TOOLTIP},
    {"customProfileImageInvalidImage", IDS_CUSTOM_PROFILE_IMAGE_INVALID_IMAGE},
};

}  // namespace

void AddCustomProfileImageData(content::WebUIDataSource* source) {
  source->AddLocalizedStrings(kCustomProfileImageStrings);
  source->AddString(
      "customProfileImageSelectedPreviewLabel",
      l10n_util::GetStringFUTF16(
          IDS_CUSTOM_PROFILE_IMAGE_SELECTED_PREVIEW_LABEL,
          l10n_util::GetStringUTF16(IDS_CUSTOM_PROFILE_IMAGE_PREVIEW_LABEL)));
  source->AddBoolean(
      "customProfileImageEnabled",
      base::FeatureList::IsEnabled(
          custom_profile_image::features::kBraveCustomProfileImage));
}

}  // namespace brave
