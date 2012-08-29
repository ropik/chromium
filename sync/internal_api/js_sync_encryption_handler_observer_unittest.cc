// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sync/internal_api/js_sync_encryption_handler_observer.h"

#include "base/basictypes.h"
#include "base/location.h"
#include "base/message_loop.h"
#include "base/values.h"
#include "sync/internal_api/public/base/model_type.h"
#include "sync/internal_api/public/util/sync_string_conversions.h"
#include "sync/internal_api/public/util/weak_handle.h"
#include "sync/js/js_event_details.h"
#include "sync/js/js_test_util.h"
#include "sync/util/cryptographer.h"
#include "sync/test/fake_encryptor.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace syncer {
namespace {

using ::testing::InSequence;
using ::testing::StrictMock;

class JsSyncEncryptionHandlerObserverTest : public testing::Test {
 protected:
  JsSyncEncryptionHandlerObserverTest() {
    js_sync_encryption_handler_observer_.SetJsEventHandler(
        mock_js_event_handler_.AsWeakHandle());
  }

 private:
  // This must be destroyed after the member variables below in order
  // for WeakHandles to be destroyed properly.
  MessageLoop message_loop_;

 protected:
  StrictMock<MockJsEventHandler> mock_js_event_handler_;
  JsSyncEncryptionHandlerObserver js_sync_encryption_handler_observer_;

  void PumpLoop() {
    message_loop_.RunAllPending();
  }
};

TEST_F(JsSyncEncryptionHandlerObserverTest, NoArgNotifiations) {
  InSequence dummy;

  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onEncryptionComplete",
                            HasDetails(JsEventDetails())));

  js_sync_encryption_handler_observer_.OnEncryptionComplete();
  PumpLoop();
}

TEST_F(JsSyncEncryptionHandlerObserverTest, OnPassphraseRequired) {
  InSequence dummy;

  DictionaryValue reason_passphrase_not_required_details;
  DictionaryValue reason_encryption_details;
  DictionaryValue reason_decryption_details;

  reason_passphrase_not_required_details.SetString(
      "reason",
      PassphraseRequiredReasonToString(REASON_PASSPHRASE_NOT_REQUIRED));
  reason_encryption_details.SetString(
      "reason",
      PassphraseRequiredReasonToString(REASON_ENCRYPTION));
  reason_decryption_details.SetString(
      "reason",
      PassphraseRequiredReasonToString(REASON_DECRYPTION));

  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onPassphraseRequired",
                           HasDetailsAsDictionary(
                               reason_passphrase_not_required_details)));
  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onPassphraseRequired",
                           HasDetailsAsDictionary(reason_encryption_details)));
  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onPassphraseRequired",
                           HasDetailsAsDictionary(reason_decryption_details)));

  js_sync_encryption_handler_observer_.OnPassphraseRequired(
      REASON_PASSPHRASE_NOT_REQUIRED,
      sync_pb::EncryptedData());
  js_sync_encryption_handler_observer_.OnPassphraseRequired(REASON_ENCRYPTION,
                                                 sync_pb::EncryptedData());
  js_sync_encryption_handler_observer_.OnPassphraseRequired(REASON_DECRYPTION,
                                                 sync_pb::EncryptedData());
  PumpLoop();
}

TEST_F(JsSyncEncryptionHandlerObserverTest, OnBootstrapTokenUpdated) {
  DictionaryValue bootstrap_token_details;
  bootstrap_token_details.SetString("bootstrapToken", "<redacted>");
  bootstrap_token_details.SetString("type", "PASSPHRASE_BOOTSTRAP_TOKEN");

  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent(
                  "onBootstrapTokenUpdated",
                  HasDetailsAsDictionary(bootstrap_token_details)));

  js_sync_encryption_handler_observer_.OnBootstrapTokenUpdated(
      "sensitive_token", PASSPHRASE_BOOTSTRAP_TOKEN);
  PumpLoop();
}

TEST_F(JsSyncEncryptionHandlerObserverTest, OnEncryptedTypesChanged) {
  DictionaryValue expected_details;
  ListValue* encrypted_type_values = new ListValue();
  const bool encrypt_everything = false;
  expected_details.Set("encryptedTypes", encrypted_type_values);
  expected_details.SetBoolean("encryptEverything", encrypt_everything);
  ModelTypeSet encrypted_types;

  for (int i = FIRST_REAL_MODEL_TYPE; i < MODEL_TYPE_COUNT; ++i) {
    ModelType type = ModelTypeFromInt(i);
    encrypted_types.Put(type);
    encrypted_type_values->Append(Value::CreateStringValue(
        ModelTypeToString(type)));
  }

  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onEncryptedTypesChanged",
                            HasDetailsAsDictionary(expected_details)));

  js_sync_encryption_handler_observer_.OnEncryptedTypesChanged(
      encrypted_types, encrypt_everything);
  PumpLoop();
}


TEST_F(JsSyncEncryptionHandlerObserverTest, OnCryptographerStateChanged) {
  DictionaryValue expected_details;
  bool expected_ready = false;
  bool expected_pending = false;
  expected_details.SetBoolean("ready", expected_ready);
  expected_details.SetBoolean("hasPendingKeys", expected_pending);
  ModelTypeSet encrypted_types;

  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onCryptographerStateChanged",
                            HasDetailsAsDictionary(expected_details)));

  FakeEncryptor encryptor;
  Cryptographer cryptographer(&encryptor);

  js_sync_encryption_handler_observer_.OnCryptographerStateChanged(
      &cryptographer);
  PumpLoop();
}

TEST_F(JsSyncEncryptionHandlerObserverTest, OnPassphraseStateChanged) {
  InSequence dummy;

  DictionaryValue passphrase_state_details;
  passphrase_state_details.SetString("passphraseState", "IMPLICIT_PASSPHRASE");
  EXPECT_CALL(mock_js_event_handler_,
              HandleJsEvent("onPassphraseStateChanged",
                            HasDetailsAsDictionary(passphrase_state_details)));

  js_sync_encryption_handler_observer_.OnPassphraseStateChanged(
      IMPLICIT_PASSPHRASE);
  PumpLoop();
}

}  // namespace
}  // namespace syncer
