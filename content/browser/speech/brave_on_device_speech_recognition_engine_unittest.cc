// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "brave/components/local_ai/core/on_device_speech_recognition.mojom.h"
#include "components/speech/audio_buffer.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/speech_recognition_session_config.h"
#include "content/public/common/content_client.h"
#include "content/public/test/test_renderer_host.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/mojo/mojom/speech_recognizer.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/on_device_model/public/mojom/on_device_model.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

// gn check cannot resolve these: both headers are sources of //content/browser
// (the Brave one via brave_content_browser_sources), and that target is not
// visible to targets outside content.
#include "brave/content/browser/speech/brave_on_device_speech_recognition_engine.h"  // nogncheck
#include "content/browser/speech/speech_recognition_engine.h"  // nogncheck

namespace content {

namespace {

constexpr int kSampleRateHz = 16000;

class FakeAsrSession : public local_ai::mojom::AsrSession,
                       public on_device_model::mojom::AsrStreamInput {
 public:
  FakeAsrSession() = default;
  ~FakeAsrSession() override = default;

  mojo::PendingRemote<local_ai::mojom::AsrSession> BindRemote() {
    return session_receiver_.BindNewPipeAndPassRemote();
  }

  // local_ai::mojom::AsrSession:
  void Start(
      on_device_model::mojom::AsrStreamOptionsPtr options,
      mojo::PendingReceiver<on_device_model::mojom::AsrStreamInput> stream,
      mojo::PendingRemote<on_device_model::mojom::AsrStreamResponder> responder)
      override {
    ++start_count_;
    sample_rate_hz_ = static_cast<int>(options->sample_rate_hz);
    stream_receiver_.Bind(std::move(stream));
    responder_.Bind(std::move(responder));
  }

  // on_device_model::mojom::AsrStreamInput:
  void AddAudioChunk(on_device_model::mojom::AudioDataPtr data) override {
    audio_chunks_.push_back(std::move(data));
  }

  void SendResult(const std::string& transcript, bool is_final) {
    std::vector<on_device_model::mojom::SpeechRecognitionResultPtr> results;
    results.push_back(on_device_model::mojom::SpeechRecognitionResult::New(
        transcript, is_final));
    responder_->OnResponse(std::move(results));
  }

  void CloseResponder() { responder_.reset(); }

  // Completes only once the engine has bound its end of the session pipe, so
  // it is a positive check that OnAsrSessionReady ran.
  void WaitUntilEngineBoundSession() { session_receiver_.FlushForTesting(); }

  int start_count() const { return start_count_; }
  int sample_rate_hz() const { return sample_rate_hz_; }
  bool responder_connected() const { return responder_.is_connected(); }
  const std::vector<on_device_model::mojom::AudioDataPtr>& audio_chunks()
      const {
    return audio_chunks_;
  }

  void set_stream_disconnect_handler(base::OnceClosure closure) {
    stream_receiver_.set_disconnect_handler(std::move(closure));
  }
  void set_session_disconnect_handler(base::OnceClosure closure) {
    session_receiver_.set_disconnect_handler(std::move(closure));
  }

 private:
  int start_count_ = 0;
  int sample_rate_hz_ = 0;
  std::vector<on_device_model::mojom::AudioDataPtr> audio_chunks_;
  mojo::Receiver<local_ai::mojom::AsrSession> session_receiver_{this};
  mojo::Receiver<on_device_model::mojom::AsrStreamInput> stream_receiver_{this};
  mojo::Remote<on_device_model::mojom::AsrStreamResponder> responder_;
};

// Hands out a session, or refuses when given no fake, the way
// BraveContentBrowserClient does when no model is installed.
class FakeContentBrowserClient : public ContentBrowserClient {
 public:
  explicit FakeContentBrowserClient(FakeAsrSession* session)
      : session_(session) {}

  mojo::PendingRemote<local_ai::mojom::AsrSession> GetAsrSession() override {
    auto pending = session_
                       ? session_->BindRemote()
                       : mojo::PendingRemote<local_ai::mojom::AsrSession>();
    requested_.SetValue();
    return pending;
  }

  // Waits for the engine's UI thread hop to reach the embedder.
  [[nodiscard]] bool WaitUntilRequested() { return requested_.Wait(); }

 private:
  raw_ptr<FakeAsrSession> session_;
  base::test::TestFuture<void> requested_;
};

// Records what the engine reported upward. SpeechRecognizerImpl is the real
// delegate, and an empty result set is how it is told to end recognition.
class TestDelegate : public SpeechRecognitionEngine::Delegate {
 public:
  void OnSpeechRecognitionEngineResults(
      const std::vector<media::mojom::WebSpeechRecognitionResultPtr>& results)
      override {
    ++results_count_;
    last_results_empty_ = results.empty();
    last_transcript_.clear();
    if (!results.empty() && !results[0]->hypotheses.empty()) {
      last_transcript_ =
          base::UTF16ToUTF8(results[0]->hypotheses[0]->utterance);
      last_result_provisional_ = results[0]->is_provisional;
    }
  }
  void OnSpeechRecognitionEngineEndOfUtterance() override {}
  void OnSpeechRecognitionEngineError(
      const media::mojom::SpeechRecognitionError& error) override {
    ++error_count_;
    last_error_code_ = error.code;
  }

  int results_count() const { return results_count_; }
  int error_count() const { return error_count_; }
  bool last_results_empty() const { return last_results_empty_; }
  const std::string& last_transcript() const { return last_transcript_; }
  bool last_result_provisional() const { return last_result_provisional_; }
  media::mojom::SpeechRecognitionErrorCode last_error_code() const {
    return last_error_code_;
  }

 private:
  int results_count_ = 0;
  int error_count_ = 0;
  bool last_results_empty_ = false;
  bool last_result_provisional_ = false;
  std::string last_transcript_;
  media::mojom::SpeechRecognitionErrorCode last_error_code_ =
      media::mojom::SpeechRecognitionErrorCode::kNone;
};

}  // namespace

class BraveOnDeviceSpeechRecognitionEngineTest
    : public RenderViewHostTestHarness {
 public:
  void TearDown() override {
    engine_.reset();
    if (old_client_) {
      SetBrowserClientForTesting(old_client_);
      old_client_ = nullptr;
    }
    RenderViewHostTestHarness::TearDown();
  }

 protected:
  // Builds the engine against a client that hands out `session`, or refuses
  // when it is null. The session is asked for on the UI thread, so it has not
  // arrived when this returns.
  void CreateEngine(FakeAsrSession* session) {
    client_ = std::make_unique<FakeContentBrowserClient>(session);
    old_client_ = SetBrowserClientForTesting(client_.get());

    SpeechRecognitionSessionConfig config;
    engine_ = std::make_unique<BraveOnDeviceSpeechRecognitionEngine>(config);
    engine_->set_delegate(&delegate_);
  }

  void SetAudioParameters() {
    engine_->SetAudioParameters(
        media::AudioParameters(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                               media::ChannelLayoutConfig::Mono(),
                               kSampleRateHz, kSampleRateHz / 100));
  }

  // Brings the engine to the point where the worker stream is running.
  [[nodiscard]] bool StartSession(FakeAsrSession& session) {
    CreateEngine(&session);
    SetAudioParameters();
    return base::test::RunUntil([&] { return session.start_count() == 1; });
  }

  // One chunk of mono 16 bit audio, the shape SpeechRecognizerImpl feeds in.
  scoped_refptr<AudioChunk> MakeAudioChunk(base::span<const int16_t> samples) {
    return base::MakeRefCounted<AudioChunk>(base::as_byte_span(samples),
                                            sizeof(int16_t));
  }

  TestDelegate delegate_;
  std::unique_ptr<BraveOnDeviceSpeechRecognitionEngine> engine_;
  std::unique_ptr<FakeContentBrowserClient> client_;

 private:
  raw_ptr<ContentBrowserClient> old_client_ = nullptr;
};

// The worker stream needs both the session remote and the audio parameters,
// which arrive asynchronously in either order. This order also covers
// SetAudioParameters not reaching the base class's null Core, which would
// DCHECK.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest, StartsWhenSessionArrivesLast) {
  FakeAsrSession session;
  CreateEngine(&session);
  SetAudioParameters();

  ASSERT_TRUE(base::test::RunUntil([&] { return session.start_count() == 1; }));
  EXPECT_EQ(kSampleRateHz, session.sample_rate_hz());
}

TEST_F(BraveOnDeviceSpeechRecognitionEngineTest,
       StartsWhenAudioParametersArriveLast) {
  FakeAsrSession session;
  CreateEngine(&session);

  ASSERT_TRUE(client_->WaitUntilRequested());
  session.WaitUntilEngineBoundSession();
  ASSERT_EQ(0, session.start_count());

  SetAudioParameters();

  ASSERT_TRUE(base::test::RunUntil([&] { return session.start_count() == 1; }));
  EXPECT_EQ(kSampleRateHz, session.sample_rate_hz());
}

// Tests that a second set of audio parameters does not start a second stream.
// The guard is synchronous, so nothing needs to be waited on here.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest, StartsOnlyOnce) {
  FakeAsrSession session;
  ASSERT_TRUE(StartSession(session));

  SetAudioParameters();

  EXPECT_EQ(1, session.start_count());
}

// Tests that a refused session is silent, and that the end of audio then ends
// recognition the way upstream does, with an empty result set.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest, RefusedSessionEndsOnAudioEnd) {
  CreateEngine(nullptr);
  SetAudioParameters();
  ASSERT_TRUE(client_->WaitUntilRequested());

  engine_->AudioChunksEnded();

  EXPECT_EQ(1, delegate_.results_count());
  EXPECT_TRUE(delegate_.last_results_empty());
  EXPECT_EQ(0, delegate_.error_count());
}

// Tests that audio reaches the worker through the stream this engine created,
// at the sample rate it negotiated. The int16 to float conversion itself is
// the base class's.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest, AudioReachesTheWorker) {
  FakeAsrSession session;
  ASSERT_TRUE(StartSession(session));

  constexpr std::array<int16_t, 4> kSamples = {0, 16384, -16384, 32767};
  engine_->TakeAudioChunk(*MakeAudioChunk(kSamples));

  ASSERT_TRUE(base::test::RunUntil(
      [&] { return session.audio_chunks().size() == 1u; }));
  const auto& chunk = session.audio_chunks()[0];
  EXPECT_EQ(kSampleRateHz, chunk->sample_rate);
  EXPECT_EQ(4, chunk->frame_count);
}

// Tests that a worker which dies mid session is reported as an error and ends
// the session. AudioChunksEnded relies on this being the thing that unblocks a
// recognizer waiting for a final result that is never coming.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest, WorkerDeathReportsError) {
  FakeAsrSession session;
  ASSERT_TRUE(StartSession(session));

  base::test::TestFuture<void> session_closed;
  session.set_session_disconnect_handler(session_closed.GetCallback());
  session.CloseResponder();

  ASSERT_TRUE(
      base::test::RunUntil([&] { return delegate_.error_count() == 1; }));
  EXPECT_EQ(media::mojom::SpeechRecognitionErrorCode::kServiceNotAllowed,
            delegate_.last_error_code());
  // The error path ends recognition, which drops the session remote.
  EXPECT_TRUE(session_closed.Wait());
}

// Tests that the end of audio closes the input stream and reports nothing.
// Closing it is what makes the worker emit its final result, and reporting an
// empty result set here would end recognition before that arrived.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest,
       AudioChunksEndedClosesStreamAndKeepsResponder) {
  FakeAsrSession session;
  ASSERT_TRUE(StartSession(session));

  base::test::TestFuture<void> stream_closed;
  session.set_stream_disconnect_handler(stream_closed.GetCallback());
  engine_->AudioChunksEnded();
  ASSERT_TRUE(stream_closed.Wait());

  // The final result still has somewhere to arrive on.
  EXPECT_TRUE(session.responder_connected());
  EXPECT_EQ(0, delegate_.results_count());
}

// Tests the sequence a real session ends on. Interim results arrive while
// audio flows, the end of audio closes the input stream, the worker answers
// with its final result, and only then does ending recognition drop the
// session remote, which is what releases the worker.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest,
       FinalResultThenEndRecognition) {
  FakeAsrSession session;
  ASSERT_TRUE(StartSession(session));

  session.SendResult("partial", /*is_final=*/false);
  ASSERT_TRUE(
      base::test::RunUntil([&] { return delegate_.results_count() == 1; }));
  EXPECT_EQ("partial", delegate_.last_transcript());
  EXPECT_TRUE(delegate_.last_result_provisional());

  base::test::TestFuture<void> stream_closed;
  session.set_stream_disconnect_handler(stream_closed.GetCallback());
  engine_->AudioChunksEnded();
  ASSERT_TRUE(stream_closed.Wait());

  session.SendResult("final", /*is_final=*/true);
  ASSERT_TRUE(
      base::test::RunUntil([&] { return delegate_.results_count() == 2; }));
  EXPECT_EQ("final", delegate_.last_transcript());
  EXPECT_FALSE(delegate_.last_result_provisional());

  base::test::TestFuture<void> session_closed;
  session.set_session_disconnect_handler(session_closed.GetCallback());
  engine_->EndRecognition();
  EXPECT_TRUE(session_closed.Wait());
  EXPECT_EQ(0, delegate_.error_count());
}

// Tests that ending a session that never got a worker is harmless. Neither the
// base class's Core nor this engine's session remote was ever set.
TEST_F(BraveOnDeviceSpeechRecognitionEngineTest, EndRecognitionWithoutSession) {
  CreateEngine(nullptr);
  SetAudioParameters();
  ASSERT_TRUE(client_->WaitUntilRequested());

  engine_->EndRecognition();

  EXPECT_EQ(0, delegate_.results_count());
  EXPECT_EQ(0, delegate_.error_count());
}

}  // namespace content
