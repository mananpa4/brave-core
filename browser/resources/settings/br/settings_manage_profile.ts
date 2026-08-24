// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

import {
  RegisterPolymerTemplateModifications,
  RegisterStyleOverride,
} from 'chrome://resources/brave/polymer_overriding.js'
import {html} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js'

// <if expr="enable_custom_profile_image">
import 'chrome://resources/brave/custom_profile_image_row.js'

import {loadTimeData} from '../i18n_setup.js'
// </if>

// Brave avatar assets are organized in 7-color groups per style under
// `app/theme/default_100_percent/common/avatars`. Chromium's default 6-column
// picker splits each group across multiple rows. Force the use of 7 columns to
// keep variants visually aligned.
const kManageProfilePickerColumns = '7'

RegisterStyleOverride(
  'settings-manage-profile',
  html`
    <style include="settings-shared">
      .content {
        --cr-section-indent-width: 20px;
      }

      .cr-row.manage-profile-section {
        padding-top: var(--leo-spacing-xl) !important;
      }

      .grid-container {
        --icon-grid-gap: 22px !important;
        --icon-size: 66px !important;
      }

      .custom-profile-image-section .content {
        --icon-grid-gap: 22px;
        --icon-size: 66px;
      }
    </style>
  `,
)

// <if expr="enable_custom_profile_image">
const kCustomProfileImageRowId = 'customProfileImageRow'

function setLocalizedAttribute(element: Element, attribute: string, key: string) {
  element.setAttribute(attribute, loadTimeData.getString(key))
}

function createCustomProfileImageSection() {
  const section = document.createElement('div')
  section.classList.add(
    'cr-row', 'manage-profile-section', 'custom-profile-image-section')

  const title = document.createElement('h1')
  title.classList.add('cr-title-text')
  title.textContent = loadTimeData.getString('customProfileImageTitle')

  const content = document.createElement('div')
  content.classList.add('content')

  const row = document.createElement('br-custom-profile-image-row')
  row.id = kCustomProfileImageRowId
  row.setAttribute('surface', 'settings')
  row.setAttribute('state', 'empty')
  row.setAttribute('hide-title', '')
  setLocalizedAttribute(row, 'title-label', 'customProfileImageUpload')
  setLocalizedAttribute(row, 'replace-label', 'customProfileImageReplace')
  setLocalizedAttribute(row, 'remove-label', 'customProfileImageRemove')
  setLocalizedAttribute(
    row, 'selected-preview-label', 'customProfileImageSelectedPreviewLabel')
  setLocalizedAttribute(row, 'preview-label', 'customProfileImagePreviewLabel')
  setLocalizedAttribute(
    row, 'upload-tooltip', 'customProfileImageUploadTooltip')
  setLocalizedAttribute(
    row, 'remove-tooltip', 'customProfileImageRemoveTooltip')
  setLocalizedAttribute(
    row, 'invalid-image-label', 'customProfileImageInvalidImage')

  content.append(row)
  section.append(title, content)
  return section
}

// </if>

/**
 * Applies Brave's Manage Profile layout and optional custom-image row to the
 * Polymer template.
 *
 * The mutation is idempotent. It throws when Chromium's section anchors no
 * longer have the structure required for the insertion.
 */
function customizeManageProfileTemplate(templateContent: DocumentFragment) {
  const themeColorPicker = templateContent.querySelector(
    'cr-theme-color-picker',
  )
  if (!themeColorPicker) {
    throw new Error('[Settings] Missing Manage Profile theme color picker')
  }
  themeColorPicker.setAttribute('columns', kManageProfilePickerColumns)

  const profileAvatarSelector = templateContent.querySelector(
    'cr-profile-avatar-selector',
  )
  if (!profileAvatarSelector) {
    throw new Error('[Settings] Missing Manage Profile avatar selector')
  }
  profileAvatarSelector.setAttribute('columns', kManageProfilePickerColumns)

  // <if expr="enable_custom_profile_image">
  if (!loadTimeData.getBoolean('customProfileImageEnabled')) {
    return
  }

  // Polymer can prepare a component template more than once. Keep the
  // structural template change idempotent on repeated preparation.
  if (templateContent.getElementById(kCustomProfileImageRowId)) {
    return
  }

  const themeColorSection = themeColorPicker.closest(
    '.manage-profile-section',
  )
  if (!themeColorSection) {
    throw new Error(
      '[Settings] Missing Manage Profile theme color picker section')
  }

  const profileAvatarSection = profileAvatarSelector.closest(
    '.manage-profile-section',
  )
  if (!profileAvatarSection) {
    throw new Error('[Settings] Missing Manage Profile avatar section')
  }

  const sectionParent = profileAvatarSection.parentElement
  if (!sectionParent || themeColorSection.parentElement !== sectionParent) {
    throw new Error('[Settings] Manage Profile sections changed structure')
  }

  sectionParent.insertBefore(
    createCustomProfileImageSection(), profileAvatarSection)
  // </if>
}

RegisterPolymerTemplateModifications({
  'settings-manage-profile': customizeManageProfileTemplate,
})
