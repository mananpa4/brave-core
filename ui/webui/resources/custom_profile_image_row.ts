// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

type CustomProfileImageState = 'empty' | 'saved-active'
type CustomProfileImageSurface = 'settings'

const observedAttributes = [
  'hide-title',
  'invalid-image-label',
  'preview-label',
  'replace-label',
  'remove-label',
  'remove-tooltip',
  'selected-preview-label',
  'state',
  'surface',
  'title-label',
  'upload-tooltip',
]

/** Shared custom-profile image control with a session-only local preview. */
export class BrCustomProfileImageRowElement extends HTMLElement {
  private hasValidationError = false
  private localPreviewUrl?: string
  private uploadAttemptId = 0

  static get observedAttributes() {
    return observedAttributes
  }

  constructor() {
    super()
    this.attachShadow({mode: 'open'})
  }

  connectedCallback() {
    this.render()
  }

  disconnectedCallback() {
    this.clearLocalPreview()
  }

  attributeChangedCallback() {
    if (this.isConnected) {
      this.render()
    }
  }

  private get state(): CustomProfileImageState {
    const state = this.getAttribute('state')
    if (state === null || state === 'empty') {
      return 'empty'
    }
    if (state === 'saved-active') {
      return 'saved-active'
    }
    throw new Error(`Unsupported custom profile image state: ${state}`)
  }

  private getLabel(name: string, fallback = '') {
    return this.getAttribute(name) || fallback
  }

  private render() {
    const root = document.createElement('section')
    root.id = 'row'
    root.className = [
      `surface-${this.surface}`,
      `state-${this.state}`,
      this.shouldRenderTitle() ? 'title-visible' : 'title-hidden',
    ].join(' ')
    if (this.shouldRenderTitle()) {
      root.setAttribute('aria-labelledby', 'title')
    } else {
      root.setAttribute('aria-label', this.getLabel('title-label'))
    }

    const preview = this.createPreview()
    const text = this.createText()
    const actions = this.createActions()
    const fileError = this.createFileError()

    root.append(preview)
    if (text) {
      root.append(text)
    }
    root.append(actions)
    if (fileError) {
      root.append(fileError)
    }
    root.append(this.createFileInput())

    const style = document.createElement('style')
    style.textContent = `
      :host {
        color: var(--cr-primary-text-color, currentColor);
        display: block;
        width: 100%;
      }

      #row {
        --avatar-size: 56px;
        --selected-border: 4px;
        align-items: center;
        box-sizing: border-box;
        display: grid;
        gap: 16px;
        grid-template-columns: auto minmax(0, 1fr) auto;
        min-height: 72px;
        width: 100%;
      }

      #preview {
        --avatar-selected-color: rgba(var(--google-blue-600-rgb), .4);
        align-items: center;
        background: var(--cr-hover-background-color, rgba(0, 0, 0, .06));
        border: 1px solid var(--google-grey-300);
        border-radius: 100%;
        box-sizing: content-box;
        color: var(--cr-secondary-text-color, currentColor);
        display: inline-flex;
        flex: 0 0 auto;
        height: var(--avatar-size);
        justify-content: center;
        padding: 0;
        position: relative;
        width: var(--avatar-size);
      }

      #preview.selected {
        outline: var(--selected-border) solid var(--avatar-selected-color);
      }

      #previewImage {
        border-radius: 100%;
        height: 100%;
        object-fit: cover;
        width: 100%;
      }

      #plusIcon {
        box-sizing: border-box;
        color: var(--cr-secondary-text-color, currentColor);
        display: inline-flex;
        flex: 0 0 24px;
        height: 24px;
        min-height: 24px;
        min-width: 24px;
        position: relative;
        width: 24px;
      }

      #plusIcon::before,
      #plusIcon::after {
        background: currentColor;
        border-radius: 2px;
        content: "";
        inset: 50% auto auto 50%;
        position: absolute;
        transform: translate(-50%, -50%);
      }

      #plusIcon::before {
        height: 3px;
        width: 24px;
      }

      #plusIcon::after {
        height: 24px;
        width: 3px;
      }

      #selectedIndicator {
        --checkmark-size: 21px;
        align-items: center;
        background: var(--google-blue-600);
        border-radius: 100%;
        color: white;
        display: inline-flex;
        font-size: 13px;
        height: var(--checkmark-size);
        justify-content: center;
        padding: 1px;
        position: absolute;
        inset-inline-end: -1px;
        top: -1px;
        width: var(--checkmark-size);
        z-index: 1;
      }

      #selectedIndicator::before {
        border-bottom: 2px solid currentColor;
        border-right: 2px solid currentColor;
        content: "";
        height: 9px;
        margin-top: -2px;
        transform: rotate(45deg);
        width: 5px;
      }

      @media (prefers-color-scheme: dark) {
        #selectedIndicator {
          background: var(--google-blue-300);
          color: var(--google-grey-900);
        }
      }

      #actions {
        align-items: center;
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
        justify-content: flex-end;
      }

      #fileError {
        color: var(--cr-error-text-color, var(--google-red-600));
        font-size: 12px;
        grid-column: 1 / -1;
        line-height: 16px;
      }

      button {
        appearance: none;
        background: var(--cr-button-background-color, transparent);
        border: 1px solid var(--cr-button-border-color, rgba(0, 0, 0, .14));
        border-radius: 4px;
        box-sizing: border-box;
        color: var(--cr-primary-text-color, currentColor);
        cursor: pointer;
        font: inherit;
        -webkit-text-fill-color: currentColor;
        min-height: 32px;
        padding: 4px 16px;
      }

      #preview.upload-control {
        cursor: pointer;
      }

      #fileInput {
        display: none;
      }

      #row.surface-settings {
        --avatar-size: var(--icon-size);
        column-gap: var(--icon-grid-gap);
        --custom-profile-image-active-color:
          var(--color-button-background-prominent,
              var(--cr-fallback-color-primary, var(--google-blue-600)));
        --custom-profile-image-active-foreground-color:
          var(--color-button-foreground-prominent,
              var(--cr-fallback-color-on-primary, white));
        grid-template-areas:
          "title title"
          "preview actions";
        grid-template-columns: calc(var(--avatar-size) + 2px) auto;
        justify-content: start;
        min-height: 0;
        padding-block-start: 4px;
        padding-inline-start: 3px;
        row-gap: 18px;
      }

      #row.surface-settings.title-hidden {
        grid-template-areas: "preview actions";
        row-gap: 0;
      }

      #row.surface-settings #text {
        grid-area: title;
      }

      #row.surface-settings #title {
        font-size: 20px;
        font-weight: 600;
        line-height: 28px;
      }

      #row.surface-settings #preview {
        grid-area: preview;
        height: var(--avatar-size);
        width: var(--avatar-size);
      }

      #row.surface-settings #actions {
        gap: 10px;
        grid-area: actions;
        justify-content: flex-start;
      }

      #row.surface-settings #actions button {
        border-radius: 999px;
        font-size: 16px;
        font-weight: 500;
        min-height: 40px;
        min-width: 104px;
        opacity: 1;
        padding: 0 26px;
      }

      #row.surface-settings #uploadButton {
        background: var(--custom-profile-image-active-color);
        border-color: var(--custom-profile-image-active-color);
        color: var(--custom-profile-image-active-foreground-color);
      }

      #row.surface-settings #removeButton {
        background: transparent;
        border-color: var(--custom-profile-image-active-color);
        color: var(--custom-profile-image-active-color);
      }
    `

    this.shadowRoot!.replaceChildren(style, root)
  }

  private createPreview() {
    const isUploadControl = this.state === 'empty'
    const preview = document.createElement(isUploadControl ? 'button' : 'div')
    preview.id = 'preview'
    if (isUploadControl) {
      preview.classList.add('upload-control')
      preview.setAttribute('type', 'button')
      preview.setAttribute(
        'aria-label', this.getLabel('upload-tooltip', this.previewAriaLabel()))
      preview.addEventListener('click', () => this.openFilePicker())
    } else {
      preview.setAttribute('role', 'img')
      preview.setAttribute('aria-label', this.previewAriaLabel())
    }

    if (this.localPreviewUrl) {
      const image = document.createElement('img')
      image.id = 'previewImage'
      image.alt = ''
      image.src = this.localPreviewUrl
      preview.append(image)
    } else {
      const plusIcon = document.createElement('span')
      plusIcon.id = 'plusIcon'
      plusIcon.setAttribute('aria-hidden', 'true')
      preview.append(plusIcon)
    }

    if (this.state === 'saved-active') {
      preview.classList.add('selected')
      const selectedIndicator = document.createElement('span')
      selectedIndicator.id = 'selectedIndicator'
      selectedIndicator.setAttribute('aria-hidden', 'true')
      preview.append(selectedIndicator)
    }

    return preview
  }

  private previewAriaLabel() {
    const label = this.getLabel('preview-label')
    if (this.state !== 'saved-active') {
      return label
    }

    return this.getLabel('selected-preview-label', label)
  }

  private createText() {
    if (!this.shouldRenderTitle()) {
      return null
    }

    const text = document.createElement('div')
    text.id = 'text'

    const title = document.createElement('div')
    title.id = 'title'
    title.textContent = this.getLabel('title-label')
    text.append(title)

    return text
  }

  private createFileError() {
    if (!this.hasValidationError) {
      return null
    }

    const error = document.createElement('div')
    error.id = 'fileError'
    error.setAttribute('role', 'alert')
    error.textContent = this.getLabel('invalid-image-label')
    return error
  }

  private get surface(): CustomProfileImageSurface {
    const surface = this.getAttribute('surface') ?? 'settings'
    if (surface === 'settings') {
      return surface
    }
    throw new Error(`Unsupported custom profile image surface: ${surface}`)
  }

  private shouldRenderTitle() {
    return !this.hasAttribute('hide-title')
  }

  private createActions() {
    const actions = document.createElement('div')
    actions.id = 'actions'

    actions.append(this.createButton(
      'uploadButton',
      this.uploadButtonLabel(),
      this.getLabel('upload-tooltip'),
      () => this.openFilePicker()))

    if (this.state === 'saved-active') {
      actions.append(this.createButton(
        'removeButton',
        this.getLabel('remove-label'),
        this.getLabel('remove-tooltip'),
        () => this.clearLocalPreview()))
    }

    return actions
  }

  private createFileInput() {
    const input = document.createElement('input')
    input.id = 'fileInput'
    input.type = 'file'
    input.accept = 'image/*'
    input.addEventListener('change', () => {
      const file = input.files?.[0]
      input.value = ''
      if (!file) {
        return
      }

      void this.replaceLocalPreview(file)
    })
    return input
  }

  private uploadButtonLabel() {
    return this.state === 'saved-active'
      ? this.getLabel('replace-label')
      : this.getLabel('title-label')
  }

  private openFilePicker() {
    this.shadowRoot!.querySelector<HTMLInputElement>('#fileInput')!.click()
  }

  private async replaceLocalPreview(file: File) {
    const uploadAttemptId = ++this.uploadAttemptId
    if (!file.type.toLowerCase().startsWith('image/')) {
      // Reject unsupported MIME types before creating a preview URL.
      this.showValidationError(uploadAttemptId)
      return
    }

    const previewUrl = URL.createObjectURL(file)
    const image = new Image()
    image.src = previewUrl
    try {
      await image.decode()
    } catch {
      // Keep the current preview when the replacement file cannot be decoded.
      URL.revokeObjectURL(previewUrl)
      this.showValidationError(uploadAttemptId)
      return
    }

    if (!this.isConnected || uploadAttemptId !== this.uploadAttemptId) {
      // Do not update a disconnected row or replace a newer upload's preview.
      URL.revokeObjectURL(previewUrl)
      return
    }

    this.revokeLocalPreviewUrl()
    this.localPreviewUrl = previewUrl
    this.hasValidationError = false
    this.setAttribute('state', 'saved-active')
  }

  private clearLocalPreview() {
    ++this.uploadAttemptId
    this.hasValidationError = false
    if (!this.localPreviewUrl) {
      return
    }

    this.revokeLocalPreviewUrl()
    this.setAttribute('state', 'empty')
  }

  private showValidationError(uploadAttemptId: number) {
    if (!this.isConnected || uploadAttemptId !== this.uploadAttemptId) {
      // An old attempt must not render an error after removal or replacement.
      return
    }

    this.hasValidationError = true
    this.render()
  }

  private revokeLocalPreviewUrl() {
    if (!this.localPreviewUrl) {
      return
    }

    URL.revokeObjectURL(this.localPreviewUrl)
    this.localPreviewUrl = undefined
  }

  private createButton(
    id: string,
    label: string,
    tooltip: string,
    onClick: () => void,
  ) {
    const button = document.createElement('button')
    button.id = id
    button.type = 'button'
    button.textContent = label
    if (tooltip) {
      button.title = tooltip
      button.setAttribute('aria-label', tooltip)
    }
    button.addEventListener('click', onClick)
    return button
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'br-custom-profile-image-row': BrCustomProfileImageRowElement
  }
}

if (!customElements.get('br-custom-profile-image-row')) {
  customElements.define(
    'br-custom-profile-image-row', BrCustomProfileImageRowElement)
}
