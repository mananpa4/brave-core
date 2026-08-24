// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

(() => {
  'use strict';

  const kCommandEvent = 'brave-custom-profile-image-command';
  const kTestHostId = 'brave-custom-profile-image-test-host';

  globalThis.registerBraveSettingsManageProfileTests = async expectedRow => {
    const {
      assertEquals,
      assertFalse,
      assertNotEquals,
      assertTrue,
    } = await import('chrome://webui-test/chai_assert.js');
    const {whenAttributeIs, whenCheck} =
        await import('chrome://webui-test/test_util.js');

    function deepQuery(root, selector) {
      const direct = root.querySelector(selector);
      if (direct) {
        return direct;
      }
      for (const element of root.querySelectorAll('*')) {
        if (element.shadowRoot) {
          const found = deepQuery(element.shadowRoot, selector);
          if (found) {
            return found;
          }
        }
      }
      return null;
    }

    async function waitFor(check) {
      while (!check()) {
        await new Promise(resolve => requestAnimationFrame(resolve));
      }
    }

    function renderedColumnCount(picker) {
      const grid = picker.shadowRoot?.querySelector('cr-grid');
      const gridElement = grid?.shadowRoot?.querySelector('#grid');
      if (!gridElement) {
        return 0;
      }
      const columns = getComputedStyle(gridElement).gridTemplateColumns.trim();
      return !columns || columns === 'none' ? 0 : columns.split(/\s+/).length;
    }

    function selectedPresetKey() {
      const host = document.getElementById(kTestHostId);
      assertNotEquals(null, host);
      host.dataset.command = 'read-selected-preset-key';
      host.dispatchEvent(new Event(kCommandEvent));
      assertEquals('', host.dataset.commandError);
      return host.dataset.selectedPresetKey || '';
    }

    function createPngFile() {
      const encoded =
          'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwC' +
          'AAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=';
      const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
      return new File([bytes], 'profile.png', {type: 'image/png'});
    }

    const suiteName = expectedRow ?
        'ManageProfileCustomProfileImageEnabled' :
        'ManageProfileCustomProfileImageDefault';

    suite(suiteName, () => {
      test('ProductionManageProfileSeam', async () => {
        await waitFor(() => {
          const page = deepQuery(document, 'settings-manage-profile');
          const themePicker =
              page?.shadowRoot?.querySelector('cr-theme-color-picker');
          const avatarSelector =
              page?.shadowRoot?.querySelector('cr-profile-avatar-selector');
          const row = page?.shadowRoot?.querySelector(
              'br-custom-profile-image-row');
          return !!themePicker && !!avatarSelector &&
              renderedColumnCount(themePicker) === 7 &&
              renderedColumnCount(avatarSelector) === 7 &&
              (!expectedRow || !!row?.shadowRoot);
        });

        const manageProfile = deepQuery(document, 'settings-manage-profile');
        assertNotEquals(null, manageProfile);
        const shadowRoot = manageProfile.shadowRoot;
        assertNotEquals(null, shadowRoot);
        const themeColorPicker =
            shadowRoot.querySelector('cr-theme-color-picker');
        const profileAvatarSelector =
            shadowRoot.querySelector('cr-profile-avatar-selector');
        assertNotEquals(null, themeColorPicker);
        assertNotEquals(null, profileAvatarSelector);

        assertEquals('7', themeColorPicker.getAttribute('columns'));
        assertEquals('7', profileAvatarSelector.getAttribute('columns'));
        assertEquals(7, renderedColumnCount(themeColorPicker));
        assertEquals(7, renderedColumnCount(profileAvatarSelector));

        const themeContent = themeColorPicker.parentElement;
        const presetContent = profileAvatarSelector.parentElement;
        assertNotEquals(null, themeContent);
        assertNotEquals(null, presetContent);
        const themeStyle = getComputedStyle(themeContent);
        const presetStyle = getComputedStyle(presetContent);
        assertEquals('20px', themeStyle.paddingInlineStart);
        assertEquals(
            '22px', themeStyle.getPropertyValue('--icon-grid-gap').trim());
        assertEquals(
            '66px', themeStyle.getPropertyValue('--icon-size').trim());
        assertEquals(
            '22px', presetStyle.getPropertyValue('--icon-grid-gap').trim());
        assertEquals(
            '66px', presetStyle.getPropertyValue('--icon-size').trim());

        const rows = shadowRoot.querySelectorAll(
            'br-custom-profile-image-row');
        if (!expectedRow) {
          assertEquals(0, rows.length);
          assertFalse(!!shadowRoot.querySelector('#customProfileImageStyle'));
          return;
        }

        assertEquals(1, rows.length);
        const row = rows[0];
        const customSection = row.closest('.manage-profile-section');
        const themeSection =
            themeColorPicker.closest('.manage-profile-section');
        const presetSection =
            profileAvatarSelector.closest('.manage-profile-section');
        assertNotEquals(null, customSection);
        assertNotEquals(null, themeSection);
        assertNotEquals(null, presetSection);
        assertEquals(customSection, themeSection.nextElementSibling);
        assertEquals(presetSection, customSection.nextElementSibling);
        assertEquals(
            'Custom profile image',
            customSection.querySelector('h1').textContent.trim());

        assertEquals('settings', row.getAttribute('surface'));
        assertEquals('empty', row.getAttribute('state'));
        assertTrue(row.hasAttribute('hide-title'));
        assertEquals('Upload custom image', row.getAttribute('title-label'));
        assertEquals('Replace', row.getAttribute('replace-label'));
        assertEquals('Remove', row.getAttribute('remove-label'));
        assertEquals(
            'Custom profile image preview, selected',
            row.getAttribute('selected-preview-label'));
        assertEquals(
            'Custom profile image preview',
            row.getAttribute('preview-label'));
        assertEquals(
            'Upload a custom profile image',
            row.getAttribute('upload-tooltip'));
        assertEquals(
            'Remove custom profile image',
            row.getAttribute('remove-tooltip'));
        assertEquals(
            "Brave can't use this file. Choose another image and try again.",
            row.getAttribute('invalid-image-label'));

        const rowShadowRoot = row.shadowRoot;
        assertNotEquals(null, rowShadowRoot);
        const renderedRow = rowShadowRoot.querySelector('#row');
        assertNotEquals(null, renderedRow);
        assertEquals(
            '66px',
            getComputedStyle(renderedRow)
                .getPropertyValue('--avatar-size')
                .trim());

        const selectedPreset = selectedPresetKey();
        assertTrue(selectedPreset.length > 0);
        const input = rowShadowRoot.querySelector('#fileInput');
        assertNotEquals(null, input);
        const files = new DataTransfer();
        files.items.add(createPngFile());
        input.files = files.files;
        input.dispatchEvent(new Event('change'));
        await whenAttributeIs(row, 'state', 'saved-active');
        await whenCheck(row, () =>
          rowShadowRoot.querySelector('#previewImage') !== null);

        assertEquals(selectedPreset, selectedPresetKey());
      });
    });
  };
})();
