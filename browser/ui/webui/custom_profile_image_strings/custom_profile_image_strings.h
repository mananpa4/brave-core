/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_BROWSER_UI_WEBUI_CUSTOM_PROFILE_IMAGE_STRINGS_CUSTOM_PROFILE_IMAGE_STRINGS_H_
#define BRAVE_BROWSER_UI_WEBUI_CUSTOM_PROFILE_IMAGE_STRINGS_CUSTOM_PROFILE_IMAGE_STRINGS_H_

#include "brave/browser/ui/custom_profile_image_buildflags.h"

static_assert(BUILDFLAG(ENABLE_CUSTOM_PROFILE_IMAGE));

namespace content {
class WebUIDataSource;
}  // namespace content

namespace brave {

// Adds the shared labels, selected-preview label, and runtime feature state to
// the non-null data source.
void AddCustomProfileImageData(content::WebUIDataSource* source);

}  // namespace brave

#endif  // BRAVE_BROWSER_UI_WEBUI_CUSTOM_PROFILE_IMAGE_STRINGS_CUSTOM_PROFILE_IMAGE_STRINGS_H_
