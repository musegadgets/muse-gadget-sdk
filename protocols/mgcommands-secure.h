/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MGCOMMANDS_SECURE_H
#define MGCOMMANDS_SECURE_H

#include "mgcommands.h"

#define MG_SECURITY_VERSION 1

#define MG_ENCRYPTED_CONTROL_UUID "70DF812E-878E-456C-A514-8A7146B6248D"
#define MG_ENCRYPTED_DATA_UUID    "8DE9F24B-CF6A-470A-9940-C1ADB3608588"
#define MG_ENCRYPTED_CONTROL_UUID_LE_BYTES                                   \
  0x8d, 0x24, 0xb6, 0x46, 0x71, 0x8a, 0x14, 0xa5, 0x6c, 0x45, 0x8e, 0x87,   \
      0x2e, 0x81, 0xdf, 0x70
#define MG_ENCRYPTED_DATA_UUID_LE_BYTES                                      \
  0x88, 0x85, 0x60, 0xb3, 0xad, 0xc1, 0x40, 0x99, 0x0a, 0x47, 0x6a, 0xcf,   \
      0x4b, 0xf2, 0xe9, 0x8d

#define MG_PUBLIC_KEY_SIZE   32
#define MG_NONCE_SIZE        16
#define MG_HASH_SIZE         32 // commitments, transcript hash, MACs
#define MG_KEY_ID_SIZE       8
#define MG_FRAME_HEADER_SIZE 5
#define MG_FRAME_TAG_SIZE    16
#define MG_FRAME_OVERHEAD    (MG_FRAME_HEADER_SIZE + MG_FRAME_TAG_SIZE)

#define MG_PAIRING_TIMEOUT_S    60
#define MG_PAIRING_MAX_ATTEMPTS 5
#define MG_AUTH_MAX_FAILURES    3

/*
 * =============================================================================
 * musegadgets BLE Protocol: Session Security (DRAFT, OPTIONAL)
 * =============================================================================
 *
 * An optional layer over mgcommands.h. Devices may omit it (e.g. development
 * builds, or gadgets that rely on BLE link-layer pairing instead). A device
 * without it does not list mg_command_key_exchange, never sends
 * connection_secured, and uses only Control and Data. A device with it adds
 * the two characteristics below and follows every rule in this header.
 *
 * =============================================================================
 * Additional GATT Characteristics (in the musegadgets service)
 * =============================================================================
 *
 *   Characteristic 3: Encrypted Control
 *     UUID: 70DF812E-878E-456C-A514-8A7146B6248D
 *     Properties: Write Without Response, Notify
 *     Permissions: Write (no link encryption or authentication required)
 *     CCC Descriptor: Read, Write
 *     Description:
 *       Control, with each command or notification wrapped in one
 *       application-encrypted frame (see Encrypted frames).
 *
 *   Characteristic 4: Encrypted Data
 *     UUID: 8DE9F24B-CF6A-470A-9940-C1ADB3608588
 *     Properties: Write Without Response, Notify
 *     Permissions: Write (no link encryption or authentication required)
 *     CCC Descriptor: Read, Write
 *     Description:
 *       Data, with each payload chunk wrapped in one application-encrypted
 *       frame. The payload type is set by change_data_type on Encrypted
 *       Control.
 *
 *   The encrypted characteristics do not require BLE pairing on purpose.
 *   Session security runs above GATT, so it behaves the same on every client
 *   stack, including ones that cannot bond (e.g. Web Bluetooth). The standard
 *   services (BAS, DIS, NUS) are not covered by session security.
 *
 * =============================================================================
 * Connection flow on a secure device
 * =============================================================================
 *
 *   1. Client negotiates ATT MTU >= MG_MIN_ATT_MTU and subscribes to all
 *      four CCC descriptors.
 *   2. Client writes mg_command_request_status on Control; the device
 *      notifies change_data_type and supported_features, which lists
 *      mg_command_key_exchange.
 *   3. The client runs the three steps below. From then on, everything in
 *      mgcommands.h uses Encrypted Control and Encrypted Data in place of
 *      Control and Data, and the device notifies
 *      mg_command_connection_secured once authenticated.
 *
 * =============================================================================
 * Security
 * =============================================================================
 *
 * Every connection builds a fresh session in three steps: key exchange,
 * enable encryption, authenticate. Nothing carries over between connections
 * except the pairing key both sides store at first pairing.
 *
 * Suite mg_crypto_suite_x25519_aes256gcm_sha256:
 *   X25519 (RFC 7748), SHA-256, HKDF-SHA256 (RFC 5869), HMAC-SHA256,
 *   AES-256-GCM with a 16-byte tag.
 *
 * Step 1: Key exchange (plaintext Control)
 *
 *   M1 client -> device:
 *     [mg_command_key_exchange, mg_key_exchange_command_commit,
 *      uint8_t MG_SECURITY_VERSION, mg_crypto_suite_t suite,
 *      uint8_t commitment[32]]
 *     commitment = SHA-256(client_public[32] || client_nonce[16])
 *   M2 device -> client:
 *     [mg_command_key_exchange, mg_key_exchange_command_response,
 *      uint8_t version, mg_crypto_suite_t suite,
 *      uint8_t device_public[32], uint8_t device_nonce[16]]
 *   M3 client -> device:
 *     [mg_command_key_exchange, mg_key_exchange_command_reveal,
 *      uint8_t client_public[32], uint8_t client_nonce[16]]
 *
 *   Public keys are fresh ephemeral X25519 keys; nonces are fresh random
 *   bytes. The device checks M3 against the commitment. Committing before
 *   seeing the device's key stops a man in the middle from steering both of
 *   its sessions to the same pairing code. Failures are reported as
 *   mg_error_code_key_exchange_failed.
 *
 *   Both sides then compute:
 *     th  = SHA-256("musegadgets ble v1" || M1 || M2 || M3)
 *     ss  = X25519(own private key, peer public key); abort if all zero
 *     prk = HKDF-Extract(salt = th, IKM = ss)
 *   and derive with HKDF-Expand(prk, info, length):
 *     info                 length  use
 *     "mg1 c2d control"    32      client -> device Encrypted Control key
 *     "mg1 c2d data"       32      client -> device Encrypted Data key
 *     "mg1 d2c control"    32      device -> client Encrypted Control key
 *     "mg1 d2c data"       32      device -> client Encrypted Data key
 *     "mg1 pairing code"   4       pairing code (Step 3)
 *     "mg1 pairing key"    32      pairing key PK (Step 3)
 *     "mg1 key id"         8       pairing key ID (Step 3)
 *   M1..M3 are the complete Control payloads as sent, command byte and any
 *   trailing bytes included.
 *   Strings are ASCII without a terminator.
 *
 * Encrypted frames (Encrypted Control and Encrypted Data)
 *
 *   Byte 0:         uint8_t flags, must be 0
 *   Bytes 1-4:      uint32_t seq
 *   Bytes 5..N-17:  AES-256-GCM ciphertext
 *   Bytes N-16..:   GCM tag
 *
 *   Nonce = seq (4 bytes, little-endian) || 8 zero bytes. AAD = bytes 0-4.
 *   Each direction of each characteristic has its own key and seq. Senders
 *   start seq at 0 and add 1 per frame; a sender that would wrap disconnects.
 *   Receivers reject a seq that is not greater than the last one accepted.
 *   The plaintext is one complete Control command or notification, or one
 *   Data chunk, so a frame carries at most ATT_MTU - 3 - MG_FRAME_OVERHEAD
 *   plaintext bytes.
 *   If a frame fails (flags, seq, or tag), the receiver drops the session.
 *   The device notifies [error, mg_command_enable_encryption,
 *   mg_error_code_decrypt_failed] on Control and disconnects.
 *
 * Step 2: Enable encryption (Encrypted Control)
 *
 *   client -> device: [mg_command_enable_encryption]
 *   device -> client: [mg_command_enable_encryption]
 *
 *   Each side decrypting the other's frame confirms both derived the same
 *   keys. From here until disconnect, all traffic uses the encrypted
 *   characteristics: the device ignores writes to Control and Data and sends
 *   nothing on them. Encryption cannot be turned off within a connection.
 *
 * Step 3: Authenticate (Encrypted Control)
 *
 *   Returning client, holding (key_id, PK) from an earlier pairing:
 *     client -> device: [mg_command_authenticate,
 *                        mg_authenticate_command_prove,
 *                        uint8_t key_id[8], uint8_t client_mac[32]]
 *     device -> client: [mg_command_authenticate,
 *                        mg_authenticate_command_proof,
 *                        uint8_t device_mac[32]]
 *     client_mac = HMAC-SHA256(PK, "mg1 client auth" || th)
 *     device_mac = HMAC-SHA256(PK, "mg1 device auth" || th)
 *   The device sends proof only after verifying client_mac; otherwise, and
 *   for an unknown key_id, it notifies mg_error_code_auth_failed. The client
 *   disconnects if device_mac does not verify. A man in the middle has a
 *   different th on each side, so it cannot relay either MAC.
 *
 *   New client (first pairing):
 *     client -> device: [mg_command_authenticate,
 *                        mg_authenticate_command_pair_request,
 *                        mg_pairing_method_t method]
 *     device -> client: [mg_command_authenticate,
 *                        mg_authenticate_command_pair_pending, method]
 *     client -> device: [mg_command_authenticate,
 *                        mg_authenticate_command_pair_confirm]
 *     device -> client: [mg_command_authenticate,
 *                        mg_authenticate_command_pair_complete,
 *                        uint8_t key_id[8]]
 *   After pair_pending, both sides ask their user to confirm (see
 *   mg_pairing_method_t), and the client sends pair_confirm once its user
 *   does. The device completes only with both confirmations within
 *   MG_PAIRING_TIMEOUT_S; otherwise it notifies
 *   mg_error_code_pairing_rejected. On completion both sides store
 *   (key_id, PK) from Step 1, and the session is authenticated.
 *
 *   After either path the device notifies [mg_command_connection_secured].
 *
 * Rules:
 *   - Before encryption is enabled, plaintext Control accepts only
 *     key_exchange and request_status. Before authentication, Encrypted
 *     Control accepts only enable_encryption, authenticate, and
 *     request_status. Anything else gets mg_error_code_encryption_required
 *     or mg_error_code_authentication_required. Gestures and audio are
 *     never sent before authentication.
 *   - Key exchange and authentication each run once per connection. Sessions
 *     end at disconnect; there is no in-session rekey.
 *   - A device accepts pair_request only in pairing mode, entered by a user
 *     action on the device or while it has no stored pairings. Each key
 *     exchange completed in pairing mode counts as an attempt; after
 *     MG_PAIRING_MAX_ATTEMPTS the device leaves pairing mode. This bounds a
 *     man in the middle's odds of matching codes by retrying.
 *   - After MG_AUTH_MAX_FAILURES failed proves on a connection, the device
 *     disconnects.
 *   - A client holding a pairing for a device must refuse to continue in
 *     plaintext if that device stops listing mg_command_key_exchange.
 *   - Compare MACs and commitments in constant time. Generate keys and
 *     nonces with a CSPRNG. Keep PK in protected storage.
 *   - The token proof (mgcommands.h, Token proof) is an ordinary command
 *     here: it runs on Encrypted Control after authentication (after
 *     connection_secured), and its clear needs a match on that session.
 *
 * =============================================================================
 */

// Commands this header adds. They share mg_command_t's byte on the wire, at
// the numbers mgcommands.h reserves for them.
MG_ENUM(uint8_t, mg_secure_command_t) {
  // No params. Notified once the session is authenticated.
  mg_command_connection_secured = 6,
  // param 1: mg_key_exchange_command_t, additional params depend on
  // sub-command. Plaintext Control only. See Security, Step 1.
  mg_command_key_exchange = 0x80,
  // No params, both directions. Encrypted Control only. See Security, Step 2.
  mg_command_enable_encryption = 0x81,
  // param 1: mg_authenticate_command_t, additional params depend on
  // sub-command. Encrypted Control only. See Security, Step 3.
  mg_command_authenticate = 0x82,
};

// Typed supported_features lists this header adds:
//   [supported_features, sub_feature, mg_command_key_exchange,
//    <mg_crypto_suite_t>...]
//   [supported_features, sub_feature, mg_command_authenticate,
//    <mg_pairing_method_t>...]

// Error codes this header adds, in the range mgcommands.h reserves for them.
// They travel as mg_error_code_t.
MG_ENUM(uint16_t, mg_secure_error_code_t) {
  // Command arrived on plaintext Control but needs an encrypted session.
  mg_error_code_encryption_required = 0x0100,
  // Command arrived on Encrypted Control before authentication.
  mg_error_code_authentication_required = 0x0101,
  mg_error_code_unsupported_version = 0x0102,
  mg_error_code_unsupported_suite = 0x0103,
  // Bad commitment, invalid public key, repeated or out-of-order message
  mg_error_code_key_exchange_failed = 0x0104,
  // Bad frame flags, seq, or tag. The device then disconnects.
  mg_error_code_decrypt_failed = 0x0105,
  // Unknown key_id or wrong client_mac
  mg_error_code_auth_failed = 0x0106,
  // Device is not in pairing mode, or method is not supported
  mg_error_code_pairing_not_allowed = 0x0107,
  // User declined on the device, or MG_PAIRING_TIMEOUT_S passed
  mg_error_code_pairing_rejected = 0x0108,
};

MG_ENUM(uint8_t, mg_crypto_suite_t) {
  // X25519, HKDF-SHA256, HMAC-SHA256, AES-256-GCM (16-byte tag)
  mg_crypto_suite_x25519_aes256gcm_sha256 = 1,
};

MG_ENUM(uint8_t, mg_key_exchange_command_t) {
  // Client -> device, M1.
  // Payload: uint8_t version, mg_crypto_suite_t suite, uint8_t commitment[32]
  mg_key_exchange_command_commit = 0,
  // Device -> client, M2.
  // Payload: uint8_t version, mg_crypto_suite_t suite,
  //          uint8_t device_public[32], uint8_t device_nonce[16]
  mg_key_exchange_command_response = 1,
  // Client -> device, M3. No reply on success.
  // Payload: uint8_t client_public[32], uint8_t client_nonce[16]
  mg_key_exchange_command_reveal = 2,
};

MG_ENUM(uint8_t, mg_authenticate_command_t) {
  // Client -> device. Payload: uint8_t key_id[8], uint8_t client_mac[32]
  mg_authenticate_command_prove = 0,
  // Device -> client. Payload: uint8_t device_mac[32]
  mg_authenticate_command_proof = 1,
  // Client -> device. Payload: mg_pairing_method_t
  mg_authenticate_command_pair_request = 2,
  // Device -> client. Payload: mg_pairing_method_t
  mg_authenticate_command_pair_pending = 3,
  // Client -> device. No payload. The client's user has confirmed.
  mg_authenticate_command_pair_confirm = 4,
  // Device -> client. Payload: uint8_t key_id[8]
  mg_authenticate_command_pair_complete = 5,
  // Both directions, authenticated sessions only. No payload.
  // The device deletes the pairing this session used or created, then
  // echoes the command. The session stays authenticated until disconnect.
  mg_authenticate_command_unpair = 6,
};

MG_ENUM(uint8_t, mg_pairing_method_t) {
  // Device and client both show the 6-digit code
  //   LE32(HKDF-Expand(prk, "mg1 pairing code", 4)) % 1000000
  // with leading zeros. The user checks the codes match and confirms on
  // both. Detects a man in the middle. Needs a device display and input.
  mg_pairing_method_numeric_comparison = 1,
  // The user presses the device button to accept; no code is compared.
  // Proves physical presence but not the peer's identity, so a man in the
  // middle during pairing goes undetected. For devices without a display.
  mg_pairing_method_physical_confirm = 2,
};

#endif
