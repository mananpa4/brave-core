// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

(() => {
  'use strict';

  const hostId = 'brave-custom-profile-image-test-host';
  const commandEvent = 'brave-custom-profile-image-command';

  if (globalThis.customElements) {
    globalThis.installBraveCustomProfileImageTestDriver = async () => {
      const [{PromiseResolver}] = await Promise.all([
        import('chrome://resources/js/promise_resolver.js'),
        import('chrome://resources/brave/custom_profile_image_row.js'),
      ]);
      await customElements.whenDefined('br-custom-profile-image-row');

      document.getElementById(hostId)?.remove();
      const host = document.createElement('div');
      host.id = hostId;
      host.hidden = true;
      document.body.append(host);

      let createdUrls = [];
      let decodeMode = 'resolve';
      let originalCreateObjectURL;
      let originalDecode;
      let originalRevokeObjectURL;
      let pendingDecodes = [];
      let revokedUrls = [];
      let row;

      function syncTelemetry() {
        host.dataset.createdUrls = JSON.stringify(createdUrls);
        host.dataset.pendingDecodeCount = String(pendingDecodes.length);
        host.dataset.revokedUrls = JSON.stringify(revokedUrls);
      }

      function restore() {
        row?.remove();
        if (originalCreateObjectURL) {
          URL.createObjectURL = originalCreateObjectURL;
          URL.revokeObjectURL = originalRevokeObjectURL;
          HTMLImageElement.prototype.decode = originalDecode;
        }
        createdUrls = [];
        decodeMode = 'resolve';
        originalCreateObjectURL = undefined;
        originalDecode = undefined;
        originalRevokeObjectURL = undefined;
        pendingDecodes = [];
        revokedUrls = [];
        row = undefined;
        host.replaceChildren();
        syncTelemetry();
      }

      function createRow() {
        row = document.createElement('br-custom-profile-image-row');
        row.setAttribute('surface', 'settings');
        row.setAttribute('state', 'empty');
        row.setAttribute('title-label', 'Upload custom image');
        row.setAttribute('replace-label', 'Replace');
        row.setAttribute('remove-label', 'Remove');
        row.setAttribute(
            'selected-preview-label',
            'Custom profile image preview, selected');
        row.setAttribute(
            'preview-label', 'Custom profile image preview');
        row.setAttribute(
            'upload-tooltip', 'Upload a custom profile image');
        row.setAttribute(
            'remove-tooltip', 'Remove custom profile image');
        row.setAttribute(
            'invalid-image-label',
            "Brave can't use this file. Choose another image and try again.");
        host.append(row);
      }

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

      function readSelectedPresetKey() {
        const selector =
            deepQuery(document, 'settings-manage-profile')?.shadowRoot
                ?.querySelector('cr-profile-avatar-selector');
        const selected = selector?.selectedAvatar ??
            selector?.avatars?.find(avatar => avatar.selected);
        return selected ? `${selected.index}:${selected.url}` : '';
      }

      function setup() {
        restore();
        originalCreateObjectURL = URL.createObjectURL;
        originalRevokeObjectURL = URL.revokeObjectURL;
        originalDecode = HTMLImageElement.prototype.decode;

        URL.createObjectURL = () => {
          const url =
              `blob:brave-custom-profile-image-${createdUrls.length + 1}`;
          createdUrls.push(url);
          syncTelemetry();
          return url;
        };
        URL.revokeObjectURL = url => {
          revokedUrls.push(String(url));
          syncTelemetry();
        };
        HTMLImageElement.prototype.decode = () => {
          if (decodeMode === 'reject') {
            return Promise.reject(new DOMException('Invalid image'));
          }
          if (decodeMode === 'pending') {
            const resolver = new PromiseResolver();
            pendingDecodes.push(resolver);
            syncTelemetry();
            return resolver.promise;
          }
          if (decodeMode === 'resolve') {
            return Promise.resolve();
          }
          throw new Error(`Unknown decode mode: ${decodeMode}`);
        };
        createRow();
        syncTelemetry();
      }

      function selectImage(data) {
        const input = row.shadowRoot.querySelector('#fileInput');
        const files = new DataTransfer();
        files.items.add(new File(
            [data.contents || 'image'], data.name,
            {type: data.type || 'image/png'}));
        input.files = files.files;
        input.dispatchEvent(new Event('change'));
      }

      function validateSurface() {
        const invalidRow =
            document.createElement('br-custom-profile-image-row');
        invalidRow.setAttribute('surface', 'unsupported');
        host.dataset.validationError = '';
        const captureValidationError = event => {
          host.dataset.validationError = event.error?.message || event.message;
          event.preventDefault();
        };
        window.addEventListener('error', captureValidationError);
        host.append(invalidRow);
        window.removeEventListener('error', captureValidationError);
        invalidRow.remove();
      }

      host.addEventListener(commandEvent, () => {
        host.dataset.commandError = '';
        const data = JSON.parse(host.dataset.commandData || '{}');
        try {
          switch (host.dataset.command) {
            case 'setup':
              setup();
              break;
            case 'teardown':
              restore();
              break;
            case 'select':
              selectImage(data);
              break;
            case 'set-decode-mode':
              if (!['pending', 'reject', 'resolve'].includes(data.mode)) {
                throw new Error(`Unknown decode mode: ${data.mode}`);
              }
              decodeMode = data.mode;
              break;
            case 'resolve-decode':
              pendingDecodes[data.index].resolve();
              break;
            case 'remove':
              row.shadowRoot.querySelector('#removeButton').click();
              break;
            case 'read-selected-preset-key':
              host.dataset.selectedPresetKey = readSelectedPresetKey();
              break;
            case 'disconnect':
              row.remove();
              break;
            case 'validate-surface':
              validateSurface();
              break;
            default:
              throw new Error(`Unknown test command: ${host.dataset.command}`);
          }
        } catch (error) {
          host.dataset.commandError = error.stack || error.message;
        }
      });
    };
    return;
  }

  globalThis.registerBraveCustomProfileImageRowTests = async () => {
    const {
      assertArrayEquals,
      assertEquals,
      assertFalse,
      assertNotEquals,
      assertTrue,
    } = await import('chrome://webui-test/chai_assert.js');
    const {whenAttributeIs, whenCheck} =
        await import('chrome://webui-test/test_util.js');

    function getHost() {
      const host = document.getElementById(hostId);
      assertNotEquals(null, host);
      return host;
    }

    function command(action, data = {}) {
      const host = getHost();
      host.dataset.command = action;
      host.dataset.commandData = JSON.stringify(data);
      host.dispatchEvent(new Event(commandEvent));
      assertEquals('', host.dataset.commandError);
    }

    function getRow() {
      const row = getHost().querySelector('br-custom-profile-image-row');
      assertNotEquals(null, row);
      return row;
    }

    function getShadowRoot() {
      const shadowRoot = getRow().shadowRoot;
      assertNotEquals(null, shadowRoot);
      return shadowRoot;
    }

    function getRequiredElement(root, selector) {
      const element = root.querySelector(selector);
      assertNotEquals(null, element, `Expected ${selector}`);
      return element;
    }

    function createdUrls() {
      return JSON.parse(getHost().dataset.createdUrls);
    }

    function revokedUrls() {
      return JSON.parse(getHost().dataset.revokedUrls);
    }

    function previewUrl() {
      return getRequiredElement(
          getShadowRoot(), '#previewImage').getAttribute('src');
    }

    function selectImage(name, type = 'image/png') {
      command('select', {name, type});
    }

    async function waitForPreview(url) {
      await whenCheck(getRow(), () =>
        getShadowRoot().querySelector('#previewImage')?.getAttribute('src') ===
            url);
    }

    suite('BraveCustomProfileImageRowTests', () => {
      setup(() => command('setup'));
      teardown(() => command('teardown'));

      test('ShowsInitialControlsAndAccessibilityLabels', () => {
        const row = getRow();
        const shadowRoot = getShadowRoot();
        const root = getRequiredElement(shadowRoot, '#row');
        assertEquals('title', root.getAttribute('aria-labelledby'));
        assertEquals(
            'Upload custom image',
            getRequiredElement(shadowRoot, '#title').textContent.trim());

        const preview = getRequiredElement(shadowRoot, '#preview');
        assertEquals('button', preview.localName);
        assertEquals(
            'Upload a custom profile image',
            preview.getAttribute('aria-label'));

        const upload = getRequiredElement(shadowRoot, '#uploadButton');
        assertEquals('Upload custom image', upload.textContent.trim());
        assertEquals('Upload a custom profile image', upload.title);
        assertEquals(
            'Upload a custom profile image',
            upload.getAttribute('aria-label'));
        assertEquals(
            'image/*',
            getRequiredElement(shadowRoot, '#fileInput').accept);
        assertEquals('settings', row.getAttribute('surface'));
        assertEquals(null, shadowRoot.querySelector('#removeButton'));
        assertEquals(null, shadowRoot.querySelector('#fileError'));
      });

      test('SelectsReplacesAndRemovesAValidImage', async () => {
        const row = getRow();
        selectImage('first.png');
        await whenAttributeIs(row, 'state', 'saved-active');
        assertEquals(createdUrls()[0], previewUrl());

        const selectedPreview = getRequiredElement(getShadowRoot(), '#preview');
        assertEquals('img', selectedPreview.getAttribute('role'));
        assertEquals(
            'Custom profile image preview, selected',
            selectedPreview.getAttribute('aria-label'));
        assertEquals(
            'Replace',
            getRequiredElement(
                getShadowRoot(), '#uploadButton').textContent.trim());
        const remove = getRequiredElement(getShadowRoot(), '#removeButton');
        assertEquals('Remove', remove.textContent.trim());
        assertEquals('Remove custom profile image', remove.title);

        selectImage('second.png');
        await waitForPreview(createdUrls()[1]);
        assertArrayEquals([createdUrls()[0]], revokedUrls());

        command('remove');
        await whenAttributeIs(row, 'state', 'empty');
        assertNotEquals(null, getShadowRoot().querySelector('#plusIcon'));
        assertEquals(null, getShadowRoot().querySelector('#removeButton'));
        assertArrayEquals(createdUrls(), revokedUrls());
      });

      test('RejectsInvalidFilesAndPreservesTheActivePreview', async () => {
        const row = getRow();
        selectImage('first.png');
        await whenAttributeIs(row, 'state', 'saved-active');
        const activeUrl = previewUrl();

        selectImage('not-an-image.txt', 'text/plain');
        await whenCheck(row, () =>
          getShadowRoot().querySelector('#fileError') !== null);
        assertEquals(1, createdUrls().length);
        assertEquals(activeUrl, previewUrl());
        assertEquals('saved-active', row.getAttribute('state'));
        const mimeError = getRequiredElement(getShadowRoot(), '#fileError');
        assertEquals('alert', mimeError.getAttribute('role'));
        assertEquals(
            "Brave can't use this file. Choose another image and try again.",
            mimeError.textContent.trim());
        assertEquals(0, revokedUrls().length);

        command('set-decode-mode', {mode: 'reject'});
        selectImage('corrupt.png');
        await whenCheck(getHost(), () => revokedUrls().length === 1);
        assertEquals(activeUrl, previewUrl());
        assertArrayEquals([createdUrls()[1]], revokedUrls());

        command('set-decode-mode', {mode: 'resolve'});
        selectImage('replacement.png');
        await waitForPreview(createdUrls()[2]);
        assertEquals(null, getShadowRoot().querySelector('#fileError'));
        assertArrayEquals(
            [createdUrls()[1], createdUrls()[0]], revokedUrls());
      });

      test('KeepsTheNewestPendingSelection', async () => {
        command('set-decode-mode', {mode: 'pending'});
        selectImage('first.png');
        selectImage('second.png');
        assertEquals('2', getHost().dataset.pendingDecodeCount);

        command('resolve-decode', {index: 1});
        await whenAttributeIs(getRow(), 'state', 'saved-active');
        assertEquals(createdUrls()[1], previewUrl());

        command('resolve-decode', {index: 0});
        await whenCheck(getHost(), () => revokedUrls().length === 1);
        assertEquals(createdUrls()[1], previewUrl());
        assertArrayEquals([createdUrls()[0]], revokedUrls());
      });

      test('RemovalCancelsAPendingReplacement', async () => {
        const row = getRow();
        selectImage('active.png');
        await whenAttributeIs(row, 'state', 'saved-active');

        command('set-decode-mode', {mode: 'pending'});
        selectImage('replacement.png');
        command('remove');
        await whenAttributeIs(row, 'state', 'empty');
        assertArrayEquals([createdUrls()[0]], revokedUrls());

        command('resolve-decode', {index: 0});
        await whenCheck(getHost(), () => revokedUrls().length === 2);
        assertEquals('empty', row.getAttribute('state'));
        assertEquals(null, getShadowRoot().querySelector('#previewImage'));
        assertEquals(null, getShadowRoot().querySelector('#fileError'));
        assertArrayEquals(createdUrls(), revokedUrls());
      });

      test('DisconnectCancelsAPendingSelection', async () => {
        const row = getRow();
        const shadowRoot = getShadowRoot();
        command('set-decode-mode', {mode: 'pending'});
        selectImage('pending.png');

        command('disconnect');
        command('resolve-decode', {index: 0});
        await whenCheck(getHost(), () => revokedUrls().length === 1);
        assertEquals('empty', row.getAttribute('state'));
        assertEquals(null, shadowRoot.querySelector('#previewImage'));
        assertEquals(null, shadowRoot.querySelector('#fileError'));
        assertArrayEquals(createdUrls(), revokedUrls());
      });

      test('DisconnectReleasesTheActivePreview', async () => {
        const row = getRow();
        selectImage('active.png');
        await whenAttributeIs(row, 'state', 'saved-active');
        command('disconnect');
        assertEquals('empty', row.getAttribute('state'));
        assertArrayEquals(createdUrls(), revokedUrls());
      });

      test('UsesSelectedIndicatorColorsForCurrentColorScheme', async () => {
        const row = getRow();
        selectImage('selected.png');
        await whenAttributeIs(row, 'state', 'saved-active');
        const indicator = getRequiredElement(
            getShadowRoot(), '#selectedIndicator');
        const isDark = matchMedia('(prefers-color-scheme: dark)').matches;
        const tokenProbe = document.createElement('span');
        tokenProbe.style.backgroundColor = isDark ?
            'var(--google-blue-300)' : 'var(--google-blue-600)';
        tokenProbe.style.color = isDark ? 'var(--google-grey-900)' : 'white';
        getShadowRoot().append(tokenProbe);

        const indicatorStyle = getComputedStyle(indicator);
        const expectedStyle = getComputedStyle(tokenProbe);
        const actualBackground = indicatorStyle.backgroundColor;
        const actualForeground = indicatorStyle.color;
        const expectedBackground = expectedStyle.backgroundColor;
        const expectedForeground = expectedStyle.color;
        tokenProbe.remove();

        assertEquals(expectedBackground, actualBackground);
        assertEquals(expectedForeground, actualForeground);
      });

      test('UsesSettingsProminentButtonColorTokens', () => {
        const row = getRow();
        row.style.setProperty(
            '--color-button-background-prominent', 'rgb(112, 81, 159)');
        row.style.setProperty(
            '--color-button-foreground-prominent', 'rgb(255, 255, 255)');
        let uploadStyle = getComputedStyle(
            getRequiredElement(row.shadowRoot, '#uploadButton'));
        assertEquals('rgb(112, 81, 159)', uploadStyle.backgroundColor);
        assertEquals('rgb(255, 255, 255)', uploadStyle.color);

        row.style.setProperty(
            '--color-button-background-prominent', 'rgb(218, 205, 101)');
        row.style.setProperty(
            '--color-button-foreground-prominent', 'rgb(54, 51, 25)');
        uploadStyle = getComputedStyle(
            getRequiredElement(row.shadowRoot, '#uploadButton'));
        assertEquals('rgb(218, 205, 101)', uploadStyle.backgroundColor);
        assertEquals('rgb(54, 51, 25)', uploadStyle.color);
      });

      test('RejectsUnsupportedSurfaceValues', () => {
        command('validate-surface');
        assertEquals(
            'Unsupported custom profile image surface: unsupported',
            getHost().dataset.validationError);
      });
    });
  };
})();
