// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include "brave/content/browser/speech/brave_on_device_speech_recognition_engine.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/task/bind_post_task.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "services/on_device_model/public/mojom/on_device_model.mojom.h"

namespace content {

namespace {

// Asks the embedder for an AsrSession on the UI thread and delivers it back
// through `callback`.
void GetAsrSessionOnUI(
    base::OnceCallback<void(mojo::PendingRemote<local_ai::mojom::AsrSession>)>
        callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  std::move(callback).Run(GetContentClient()->browser()->GetAsrSession());
}

}  // namespace

BraveOnDeviceSpeechRecognitionEngine::BraveOnDeviceSpeechRecognitionEngine(
    const SpeechRecognitionSessionConfig& config)
    : OnDeviceSpeechRecognitionEngine(config) {
  GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          &GetAsrSessionOnUI,
          base::BindPostTaskToCurrentDefault(base::BindOnce(
              &BraveOnDeviceSpeechRecognitionEngine::OnAsrSessionReady,
              brave_weak_factory_.GetWeakPtr()))));
}

BraveOnDeviceSpeechRecognitionEngine::~BraveOnDeviceSpeechRecognitionEngine() =
    default;

void BraveOnDeviceSpeechRecognitionEngine::OnAsrSessionReady(
    mojo::PendingRemote<local_ai::mojom::AsrSession> pending) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);
  if (!pending.is_valid()) {
    // The embedder declined. Nothing is reported from here because the
    // recognizer ignores engine errors until it starts recording, and
    // AudioChunksEnded already ends a session that has no worker.
    return;
  }
  asr_session_.Bind(std::move(pending));
  TryStartSession();
}

void BraveOnDeviceSpeechRecognitionEngine::SetAudioParameters(
    media::AudioParameters audio_parameters) {
  // Skip the base class, which would AsyncCall a null SequenceBound and
  // DCHECK.
  SpeechRecognitionEngine::SetAudioParameters(audio_parameters);
  TryStartSession();
}

void BraveOnDeviceSpeechRecognitionEngine::TryStartSession() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);
  if (session_created_ || !asr_session_.is_bound() ||
      !audio_parameters_.IsValid()) {
    return;
  }
  session_created_ = true;

  auto options = on_device_model::mojom::AsrStreamOptions::New();
  options->sample_rate_hz = audio_parameters_.sample_rate();

  mojo::PendingRemote<on_device_model::mojom::AsrStreamInput> asr_stream;
  mojo::PendingReceiver<on_device_model::mojom::AsrStreamResponder>
      asr_stream_responder;
  asr_session_->Start(std::move(options),
                      asr_stream.InitWithNewPipeAndPassReceiver(),
                      asr_stream_responder.InitWithNewPipeAndPassRemote());

  // Hand the pipes to the base class, which owns both bindings and the
  // responder disconnect handler, and resets them in EndRecognition.
  OnAsrStreamCreated(std::move(asr_stream), std::move(asr_stream_responder));
}

void BraveOnDeviceSpeechRecognitionEngine::AudioChunksEnded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);
  // Closing the input stream is what makes the worker emit its final result,
  // so the responder stays bound to receive it. Upstream's empty result would
  // end recognition first. A worker that stays alive but never reports leaves
  // the session open until the page aborts it.
  if (asr_stream_.is_bound()) {
    asr_stream_.reset();
    return;
  }

  // No stream to close, so no final result is coming. Fall through to
  // upstream, which ends recognition with an empty result.
  OnDeviceSpeechRecognitionEngine::AudioChunksEnded();
}

void BraveOnDeviceSpeechRecognitionEngine::EndRecognition() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(main_sequence_checker_);
  OnDeviceSpeechRecognitionEngine::EndRecognition();
  // Drop the remote to tell the controller this session ended.
  asr_session_.reset();
}

}  // namespace content
