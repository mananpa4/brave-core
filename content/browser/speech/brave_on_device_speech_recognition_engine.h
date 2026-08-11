// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef BRAVE_CONTENT_BROWSER_SPEECH_BRAVE_ON_DEVICE_SPEECH_RECOGNITION_ENGINE_H_
#define BRAVE_CONTENT_BROWSER_SPEECH_BRAVE_ON_DEVICE_SPEECH_RECOGNITION_ENGINE_H_

#include "base/memory/weak_ptr.h"
#include "brave/components/local_ai/core/on_device_speech_recognition.mojom.h"
// Compiled inside the content library (via content/browser/sources.gni)
// because the base class header is content-internal.
#include "content/browser/speech/on_device_speech_recognition_engine_impl.h"
#include "content/common/content_export.h"
#include "media/base/audio_parameters.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace content {

// Subclass of Chromium's OnDeviceSpeechRecognitionEngine that takes its
// recognition stream from Brave's own model through
// ContentBrowserClient::GetAsrSession, instead of the ModelBroker session the
// base class creates.
//
// The base class's UI thread Core is not built for these sessions. Brave's
// substitution in on_device_speech_recognition_engine_impl.cc returns from the
// base constructor before creating it, leaving core_ a null SequenceBound, so
// SetAudioParameters is overridden to stop the base from calling AsyncCall on
// it.
//
// Lives on the IO thread. All Mojo bindings and WeakPtrs stay there.
class CONTENT_EXPORT BraveOnDeviceSpeechRecognitionEngine
    : public OnDeviceSpeechRecognitionEngine {
 public:
  explicit BraveOnDeviceSpeechRecognitionEngine(
      const SpeechRecognitionSessionConfig& config);
  BraveOnDeviceSpeechRecognitionEngine(
      const BraveOnDeviceSpeechRecognitionEngine&) = delete;
  BraveOnDeviceSpeechRecognitionEngine& operator=(
      const BraveOnDeviceSpeechRecognitionEngine&) = delete;
  ~BraveOnDeviceSpeechRecognitionEngine() override;

  // SpeechRecognitionEngine:
  void SetAudioParameters(media::AudioParameters audio_parameters) override;
  void AudioChunksEnded() override;
  void EndRecognition() override;

 private:
  void OnAsrSessionReady(
      mojo::PendingRemote<local_ai::mojom::AsrSession> pending);

  // Creates the worker stream once the AsrSession remote and the audio
  // parameters are both in. They arrive asynchronously and in either order, so
  // this runs after each one and does nothing until it has both.
  void TryStartSession();

  // The controller's lease on the worker. Dropping it is what tells the
  // controller that this session ended.
  mojo::Remote<local_ai::mojom::AsrSession> asr_session_;

  bool session_created_ = false;

  // Named apart from the base class's own weak_factory_.
  base::WeakPtrFactory<BraveOnDeviceSpeechRecognitionEngine>
      brave_weak_factory_{this};
};

}  // namespace content

#endif  // BRAVE_CONTENT_BROWSER_SPEECH_BRAVE_ON_DEVICE_SPEECH_RECOGNITION_ENGINE_H_
