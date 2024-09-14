/*
 * Synaptics Tudor Match-In-Sensor driver for libfprint
 *
 * Copyright (c) 2024 Francesco Circhetta, Vojtěch Pluskal
 *
 * some parts are based on work of Popax21 see:
 * https://github.com/Popax21/synaTudor/tree/rev
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "synamoc-TlsSession"

#include "tls_session.h"

#include <glib.h>
#include <openssl/conf.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <stdio.h>

#include "fpi-byte-writer.h"
#include "fpi-device.h"
#include "fpi-log.h"
#include "utils.h"

#define DEBUG_SSL TRUE

#define RANDOM_SIZE 32
#define MASTER_SECRET_SIZE 48
#define VERIFY_DATA_SIZE 12
#define MAX_SESSION_ID_SIZE 32
#define MAX_HASH_SIZE 64
#define MAX_KEY_BLOCK_SIZE 128

typedef enum
{
  HANDSHAKE_BEGIN,
  CLIENT_HELLO_SENT,
  SUITE_HANDSHAKE,
  SERVER_DONE,
  FINISHED
} HandshakePhase;

typedef struct
{
  guint8 msg_type;
  guint32 length;
  guint8 *body;
  gchar *repr;
} Handshake;

typedef struct
{
  guint8 type;
  guint16 version;
  guint16 length;
  guint8 *fragment;
  gchar *repr;
} TlsRecord;

typedef enum
{
  TLS_NULL_WITH_NULL_NULL = 0x0000,
  TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384 = 0xC02E,
} CipherSuiteType;

typedef enum
{
  RSA_SIGN = 1,
  DSA_SIGN = 2,
  RSA_FIXED_DH = 3,
  DSS_FIXED_DH = 4,
  ECDSA_SIGN = 64,
  RSA_FIXED_ECDH = 65,
  ECDSA_FIXED_ECDH = 66,
} CertificateSigType;

struct _TlsSession
{
  gboolean send_closed;
  gboolean recv_closed;

  HandshakePhase handshake_phase;
  FpiByteWriter handshake_buffer;

  guint16 server_cs;
  guint16 client_cs;
  guint16 pending_cs;
  guint8 master_secret[MASTER_SECRET_SIZE];
  guint8 client_random[RANDOM_SIZE];
  guint8 server_random[RANDOM_SIZE];

  guint64 encr_seq_num;
  guint64 decr_seq_num;
  guint8 *encr_key;
  guint8 encr_key_len;
  guint8 *decr_key;
  guint8 decr_key_len;
  guint8 *encr_iv;
  guint8 encr_iv_len;
  guint8 *decr_iv;
  guint8 decr_iv_len;

  guint16 version;

  guint8 session_id_size;
  guint8 *session_id;

  guint16 suites_size;
  guint8 *suites;

  FpiByteWriter send_buffer;

  FpiByteWriter content_buffer;
  guint8 content_buffer_type;

  FpiByteWriter application_data;

  char *hash_algo;

  // FIXME: what to fix?
  guint8 cert_request;
  SensorPairingData *pairing_data;
  // gboolean established;
  // gboolean remote_established;
};

void tls_session_free(TlsSession *self)
{
  // FIXME: session ID does not get freed somehow
  g_free(self->session_id);
  g_free(self->suites);

  g_free(self->encr_key);
  g_free(self->decr_key);

  g_free(self->encr_iv);
  g_free(self->decr_iv);

  fpi_byte_writer_reset(&self->send_buffer);
  fpi_byte_writer_reset(&self->content_buffer);
  fpi_byte_writer_reset(&self->application_data);
  fpi_byte_writer_reset(&self->handshake_buffer);

  g_free(self);
}

static gboolean tls_prf(guint8 *master, gsize master_size, gchar *label,
                        gsize label_size, guint8 *seed, gsize seed_size,
                        guint8 *out, gsize outsize, char *hash_algo,
                        GError **error)
{
  g_autoptr(EVP_KDF) kdf = NULL;
  g_autoptr(EVP_KDF_CTX) kctx;
  OSSL_PARAM params[5], *p = params;

  kdf = EVP_KDF_fetch(NULL, "TLS1-PRF", NULL);
  kctx = EVP_KDF_CTX_new(kdf);

  *p++ = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, hash_algo,
                                          strlen(hash_algo));
  *p++ = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, master,
                                           master_size);
  *p++ =
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SEED, label, label_size);
  *p++ =
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SEED, seed, seed_size);
  *p = OSSL_PARAM_construct_end();

  if (!EVP_KDF_derive(kctx, out, outsize, params))
  {
    g_propagate_error(error, fpi_device_error_new_msg(
                                 FP_DEVICE_ERROR_GENERAL, "TLS_PRF failed: %s",
                                 ERR_error_string(ERR_get_error(), NULL)));
    return FALSE;
  }

  return TRUE;
}

static void tls_handshake_free(Handshake *msg)
{
  g_free(msg->body);
  g_free(msg->repr);
  g_free(msg);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Handshake, tls_handshake_free);

static void tls_plaintext_free(TlsRecord *record)
{
  g_free(record->fragment);
  g_free(record->repr);
  g_free(record);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(TlsRecord, tls_plaintext_free);

static gboolean tls_session_handshake_hash(TlsSession *self, guint8 *out,
                                           gsize *outlen)
{
  g_autoptr(EVP_MD_CTX) ctx = EVP_MD_CTX_new();
  guint hash_len;

  gsize hdata_len = fpi_byte_writer_get_pos(&self->handshake_buffer);
  g_autofree guint8 *hdata =
      fpi_byte_writer_reset_and_get_data(&self->handshake_buffer);
  fpi_byte_writer_init(&self->handshake_buffer);
  fpi_byte_writer_put_data(&self->handshake_buffer, hdata, hdata_len);

  if (ctx == NULL || !EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) ||
      !EVP_DigestUpdate(ctx, hdata, hdata_len) ||
      !EVP_DigestFinal_ex(ctx, out, &hash_len))
    g_assert_not_reached();

  *outlen = hash_len;

  return TRUE;
}

static gboolean gcm_encrypt(unsigned char *plaintext, int plaintext_len,
                            unsigned char *aad, int aad_len, unsigned char *key,
                            unsigned char *iv, int iv_len,
                            unsigned char *ciphertext, int *ciphertext_len,
                            unsigned char *tag)
{
  int len, discard_len;
  // Create and initialise the context
  g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();

  if (ctx == NULL ||
      // Initialise the encryption operation
      !EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) ||
      // Set IV length
      !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL) ||
      // Initialise key and IV
      !EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) ||
      // Provide any AAD data
      !EVP_EncryptUpdate(ctx, NULL, &discard_len, aad, aad_len) ||
      // Provide the message to be encrypted, and obtain the encrypted output
      !EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len) ||
      // Finalise: note get no output for GCM
      !EVP_EncryptFinal_ex(ctx, ciphertext + len, &discard_len) ||
      // Get the tag
      !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag))
    g_assert_not_reached();

  *ciphertext_len = len;

  return TRUE;
}

static gboolean gcm_decrypt(unsigned char *ciphertext, int ciphertext_len,
                            unsigned char *aad, int aad_len, unsigned char *tag,
                            unsigned char *key, unsigned char *iv, int iv_len,
                            unsigned char *plaintext, gsize *plaintext_len)
{
  int len, discard_len;

  // Create and initialise the context
  g_autoptr(EVP_CIPHER_CTX) ctx = EVP_CIPHER_CTX_new();

  if (ctx == NULL ||
      // Initialise the decryption operation
      !EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) ||
      // Set IV length
      !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv_len, NULL) ||
      // Initialise key and IV
      !EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv) ||
      // Provide any AAD data
      !EVP_DecryptUpdate(ctx, NULL, &discard_len, aad, aad_len) ||
      // Provide the message to be decrypted, and obtain the plaintext output
      !EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len) ||
      /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
      !EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag) ||
      // Finalise the decryption
      !EVP_DecryptFinal_ex(ctx, plaintext + len, &discard_len))
    g_assert_not_reached();

  *plaintext_len = len;

  return TRUE;
}

static gboolean tls_session_encrypt(TlsSession *self, guint8 type,
                                    guint8 *ptext, gsize ptext_size,
                                    guint8 **ctext, gsize *ctext_size,
                                    GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;

  fpi_byte_writer_init(&writer);
  written &= fpi_byte_writer_put_uint8(&writer, type);
  written &= fpi_byte_writer_put_uint16_le(&writer, self->version);

  switch (self->client_cs)
  {
    case TLS_NULL_WITH_NULL_NULL:
    {
      written &= fpi_byte_writer_put_uint16_be(&writer, ptext_size);
      written &= fpi_byte_writer_put_data(&writer, ptext, ptext_size);
      g_assert(written);

      *ctext_size = fpi_byte_writer_get_pos(&writer);
      *ctext = fpi_byte_writer_reset_and_get_data(&writer);
      break;
    }
    case TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384:
    {
      g_autofree guint8 *gcm_iv;

      // Create random nonce & add it to output
      guint8 nonce[8];
      RAND_bytes(nonce, 8);

      // encr_iv + nonce
      gcm_iv = g_malloc(12);
      memcpy(gcm_iv, self->encr_iv, 4);
      memcpy(gcm_iv + self->encr_iv_len, nonce, 8);

      // additional_data = seq_num + TLSCompressed.type + TLSCompressed.version
      // + TLSCompressed.length;
      FpiByteWriter adwriter;
      fpi_byte_writer_init(&adwriter);
      written &= fpi_byte_writer_put_uint64_be(&adwriter, self->encr_seq_num);
      written &= fpi_byte_writer_put_uint8(&adwriter, type);
      written &= fpi_byte_writer_put_uint16_be(&adwriter, self->version);
      written &= fpi_byte_writer_put_uint16_be(&adwriter, ptext_size);

      g_assert(written);

      gsize additional_size = fpi_byte_writer_get_pos(&adwriter);
      g_autofree guint8 *additional =
          fpi_byte_writer_reset_and_get_data(&adwriter);

      gint cdata_size = ptext_size + 16;
      g_autofree guint8 *cdata = g_malloc(cdata_size);

      /* Buffer for the tag */
      unsigned char tag[16];

      gcm_encrypt(ptext, ptext_size, additional, additional_size,
                  self->encr_key, gcm_iv, 12, cdata, &cdata_size, tag);

      written &= fpi_byte_writer_put_uint16_be(&writer, 8 + cdata_size + 16);
      written &= fpi_byte_writer_put_data(&writer, nonce, 8);
      written &= fpi_byte_writer_put_data(&writer, cdata, cdata_size);
      written &= fpi_byte_writer_put_data(&writer, tag, 16);
      g_assert(written);

      self->encr_seq_num += 1;
      *ctext_size = fpi_byte_writer_get_pos(&writer);
      *ctext = fpi_byte_writer_reset_and_get_data(&writer);
      break;
    }
    default:
    {
      local_error = fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,
                                             "Cipher suite not supported");
      g_propagate_error(error, local_error);
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean tls_session_decrypt(TlsSession *self, guint8 type,
                                    guint16 version, guint8 *ctext,
                                    gsize ctext_size, guint8 **ptext,
                                    gsize *ptext_size, GError **error)
{
  switch (self->server_cs)
  {
    case TLS_NULL_WITH_NULL_NULL:
    {
      *ptext = g_memdup2(ctext, ctext_size);
      *ptext_size = ctext_size;
    }
    break;
    case TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384:
    {
      g_autofree guint8 *gcm_iv;
      gsize gcm_iv_size;
      FpiByteReader reader;
      gboolean read_ok = TRUE;

      g_autofree guint8 *nonce;
      g_autofree guint8 *cdata;
      g_autofree guint8 *tag;

      fpi_byte_reader_init(&reader, ctext, ctext_size);
      read_ok &= fpi_byte_reader_dup_data(&reader, 8, &nonce);
      read_ok &= fpi_byte_reader_dup_data(&reader, ctext_size - 8 - 16, &cdata);
      read_ok &= fpi_byte_reader_dup_data(&reader, 16, &tag);
      g_assert(read_ok);

      // encr_iv + nonce
      gcm_iv_size = 8 + self->decr_iv_len;
      gcm_iv = g_malloc(gcm_iv_size);
      memcpy(gcm_iv, self->decr_iv, self->decr_iv_len);
      memcpy(gcm_iv + self->decr_iv_len, nonce, 8);

      FpiByteWriter adwriter;
      gboolean written = TRUE;

      fpi_byte_writer_init(&adwriter);
      written &= fpi_byte_writer_put_uint64_be(&adwriter, self->decr_seq_num);
      written &= fpi_byte_writer_put_uint8(&adwriter, type);
      written &= fpi_byte_writer_put_uint16_be(&adwriter, version);
      written &= fpi_byte_writer_put_uint16_be(&adwriter, ctext_size - 8 - 16);

      g_assert(written);

      gsize additional_size = fpi_byte_writer_get_pos(&adwriter);
      g_autofree guint8 *additional =
          fpi_byte_writer_reset_and_get_data(&adwriter);

      *ptext_size = ctext_size - 8 - 16;
      *ptext = g_malloc(*ptext_size);

      gcm_decrypt(cdata, ctext_size - 8 - 16, additional, additional_size, tag,
                  self->decr_key, gcm_iv, gcm_iv_size, *ptext, ptext_size);

      self->decr_seq_num += 1;
    }
    break;
    default:
    {
      g_propagate_error(error,
                        fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO,
                                                 "Cipher suite not supported"));
      return FALSE;
    }
  }

  return TRUE;
}

static gboolean tls_session_flush_content_buffer(TlsSession *self,
                                                 GError **error)
{
  GError *local_error = NULL;
  g_autofree guint8 *plaintext = NULL;
  g_autofree guint8 *ciphertext = NULL;
  gsize plaintext_size, ciphertext_size;

  if (self->content_buffer_type != 0)
  {
    plaintext_size = fpi_byte_writer_get_pos(&self->content_buffer);
    plaintext = fpi_byte_writer_reset_and_get_data(&self->content_buffer);

    fpi_byte_writer_init(&self->content_buffer);

    if (!tls_session_encrypt(self, self->content_buffer_type, plaintext,
                             plaintext_size, &ciphertext, &ciphertext_size,
                             &local_error))
    {
      g_propagate_error(error, local_error);
      return FALSE;
    }

    fpi_byte_writer_put_data(&self->send_buffer, ciphertext, ciphertext_size);

    self->content_buffer_type = 0;
  }

  return TRUE;
}

gboolean tls_session_flush_send_buffer(TlsSession *self, guint8 **data,
                                       gsize *size, GError **error)
{
  GError *local_error = NULL;

  if (!tls_session_flush_content_buffer(self, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  *size = fpi_byte_writer_get_pos(&self->send_buffer);
  *data = fpi_byte_writer_reset_and_get_data(&self->send_buffer);
  fpi_byte_writer_init(&self->send_buffer);

  return TRUE;
}

static gboolean tls_session_send(TlsSession *self, TlsRecord *record,
                                 GError **error)
{
  GError *local_error = NULL;
  g_assert(!self->send_closed);
#if DEBUG_SSL
  fp_dbg("-> %s", record->repr);
#endif

  if (self->content_buffer_type != 0 &&
      self->content_buffer_type != record->type)
  {
    if (!tls_session_flush_content_buffer(self, &local_error))
    {
      g_propagate_error(error, local_error);
      return FALSE;
    }
  }

  fpi_byte_writer_put_data(&self->content_buffer, record->fragment,
                           record->length);
  self->content_buffer_type = record->type;

  return TRUE;
}

static gboolean tls_session_send_alert(TlsSession *self, guint8 level,
                                       guint8 description, GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;

  g_autoptr(TlsRecord) record = g_new0(TlsRecord, 1);

  fpi_byte_writer_init(&writer);
  written &= fpi_byte_writer_put_uint8(&writer, level);
  written &= fpi_byte_writer_put_uint8(&writer, description);

  g_assert(written);

  record->type = SSL3_RT_ALERT;
  record->length = fpi_byte_writer_get_pos(&writer);
  record->fragment = fpi_byte_writer_reset_and_get_data(&writer);

  asprintf(&record->repr, "TlsAlert(level=\"%s\", description=\"%s\")",
           level == SSL3_AL_WARNING ? "warning" : "fatal",
           SSL_alert_desc_string_long(description));

  if (!tls_session_send(self, record, NULL))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  if (level == SSL3_AL_WARNING) self->send_closed = TRUE;

  return TRUE;
}

gboolean tls_session_close(TlsSession *self, GError **error)
{
  GError *local_error = NULL;

  if (self->send_closed) return TRUE;

  // Send close_notify alert
  if (!tls_session_send_alert(self, SSL3_AL_WARNING, SSL3_AD_CLOSE_NOTIFY,
                              &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  self->send_closed = TRUE;

  return TRUE;
}

static gboolean tls_session_send_handshake_msg(TlsSession *self, Handshake *msg,
                                               GError **error)
{
  GError *local_error = NULL;
  g_autoptr(TlsRecord) record = g_new(TlsRecord, 1);

  FpiByteWriter writer;
  gboolean written = TRUE;

  fpi_byte_writer_init(&writer);
  written &= fpi_byte_writer_put_uint8(&writer, msg->msg_type);
  written &= fpi_byte_writer_put_uint24_be(&writer, msg->length);
  written &= fpi_byte_writer_put_data(&writer, msg->body, msg->length);

  g_assert(written);

  record->type = SSL3_RT_HANDSHAKE;
  record->version = TLS1_2_VERSION;
  record->length = fpi_byte_writer_get_pos(&writer);
  record->fragment = fpi_byte_writer_reset_and_get_data(&writer);
  asprintf(&record->repr, "HandshakeMessage(type=0x%02x, content=%s)",
           msg->msg_type, msg->repr);

  // Update hash
  // BROKEN The windows driver only updates it when the message isn't
  // "Finished"
  if (msg->msg_type != SSL3_MT_FINISHED)
  {
    written &= fpi_byte_writer_put_data(&self->handshake_buffer,
                                        record->fragment, record->length);
    g_assert(written);
  }

  if (!tls_session_send(self, record, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_send_client_hello(TlsSession *self, GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;

  fpi_byte_writer_init(&writer);
  written &= fpi_byte_writer_put_uint16_be(&writer, self->version);

  written &=
      fpi_byte_writer_put_data(&writer, self->client_random, RANDOM_SIZE);

  written &= fpi_byte_writer_put_uint8(&writer, self->session_id_size);
  written &= fpi_byte_writer_put_data(&writer, self->session_id,
                                      self->session_id_size);

  written &= fpi_byte_writer_put_uint16_be(&writer, self->suites_size);
  written &= fpi_byte_writer_put_data(&writer, self->suites, self->suites_size);

  // BROKEN The windows driver doesn't advertise the NULL compression method
  written &= fpi_byte_writer_put_uint8(&writer, 0);

  guint8 supported_groups_data[4] = {0x00, 0x02, 0x00, 0x17};
  written &= fpi_byte_writer_put_uint16_be(&writer, 0x0a);
  written &=
      fpi_byte_writer_put_uint16_be(&writer, sizeof(supported_groups_data));
  written &= fpi_byte_writer_put_data(&writer, supported_groups_data,
                                      sizeof(supported_groups_data));

  guint8 ec_point_formats_data[2] = {0x01, 0x00};
  written &= fpi_byte_writer_put_uint16_be(&writer, 0x0b);
  written &=
      fpi_byte_writer_put_uint16_be(&writer, sizeof(ec_point_formats_data));
  written &= fpi_byte_writer_put_data(&writer, ec_point_formats_data,
                                      sizeof(ec_point_formats_data));

  g_assert(written);

  g_autoptr(Handshake) msg = g_new(Handshake, 1);

  msg->msg_type = SSL3_MT_CLIENT_HELLO;
  msg->length = fpi_byte_writer_get_pos(&writer);
  msg->body = fpi_byte_writer_reset_and_get_data(&writer);

  g_autofree gchar *ses_id_str =
      bin2hex(self->session_id, self->session_id_size);
  g_autofree gchar *rand_str = bin2hex(self->client_random, RANDOM_SIZE);
  asprintf(&msg->repr,
           "ClientHello(ver=0x%04x, rand=%s, ses_id=%s, cipher_suites=TODO, "
           "compr_methods=[], extensions=TODO)",
           self->version, rand_str, ses_id_str);

  if (!tls_session_send_handshake_msg(self, msg, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_send_finished(TlsSession *self, guint8 *verify_data,
                                          GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;
  const guint verify_data_size = 12;

  fpi_byte_writer_init_with_size(&writer, verify_data_size, TRUE);
  written &= fpi_byte_writer_put_data(&writer, verify_data, verify_data_size);

  g_assert(written);

  g_autoptr(Handshake) msg = g_new(Handshake, 1);

  msg->msg_type = SSL3_MT_FINISHED;
  msg->length = fpi_byte_writer_get_pos(&writer);
  msg->body = fpi_byte_writer_reset_and_get_data(&writer);
  g_autofree gchar *verify_data_str = bin2hex(verify_data, verify_data_size);
  asprintf(&msg->repr, "Finished(verify_data=%s)", verify_data_str);

  if (!tls_session_send_handshake_msg(self, msg, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_send_certificate(TlsSession *self, GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;

  fpi_byte_writer_init(&writer);

  // TlsHandshakeCertificate
  written &= fpi_byte_writer_put_uint24_be(&writer, CERTIFICATE_SIZE);

  // TlsCertificate
  // BROKEN The windows driver has two garbage bytes after the length field
  written &= fpi_byte_writer_put_uint24_be(&writer, CERTIFICATE_SIZE);
  written &= fpi_byte_writer_fill(&writer, 0, 2);  // Garbage
  written &= fpi_byte_writer_put_data(
      &writer, self->pairing_data->client_cert_raw, CERTIFICATE_SIZE);

  g_assert(written);

  g_autoptr(Handshake) msg = g_new(Handshake, 1);

  msg->msg_type = SSL3_MT_CERTIFICATE;
  msg->length = fpi_byte_writer_get_pos(&writer);
  msg->body = fpi_byte_writer_reset_and_get_data(&writer);

  g_autofree gchar *cert_str =
      bin2hex(self->pairing_data->remote_cert_raw, CERTIFICATE_SIZE);
  asprintf(&msg->repr, "Certificate(cert=TlsCertificate(data=%s))", cert_str);

  if (!tls_session_send_handshake_msg(self, msg, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_send_client_key_exchange(TlsSession *self,
                                                     guint8 *key, gsize key_len,
                                                     GError **error)
{
  GError *local_error = NULL;

  g_autoptr(Handshake) msg = g_new(Handshake, 1);

  msg->msg_type = SSL3_MT_CLIENT_KEY_EXCHANGE;
  msg->length = key_len;
  msg->body = key;
  g_autofree gchar *key_str = bin2hex(key, key_len);
  asprintf(&msg->repr, "ClientKeyExchange(data=%s)", key_str);

  if (!tls_session_send_handshake_msg(self, msg, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_send_certificate_verify(TlsSession *self,
                                                    guint8 *signature,
                                                    gsize signature_len,
                                                    GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;

  fpi_byte_writer_init(&writer);
  written &= fpi_byte_writer_put_data(&writer, signature, signature_len);

  g_assert(written);

  g_autoptr(Handshake) msg = g_new(Handshake, 1);

  msg->msg_type = SSL3_MT_CERTIFICATE_VERIFY;
  msg->length = fpi_byte_writer_get_pos(&writer);
  msg->body = fpi_byte_writer_reset_and_get_data(&writer);

  g_autofree gchar *sign_str = bin2hex(signature, signature_len);
  asprintf(&msg->repr, "CertificateVerify(signed_hash=%s)", sign_str);

  if (!tls_session_send_handshake_msg(self, msg, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_send_change_cipher_spec(TlsSession *self,
                                                    GError **error)
{
  GError *local_error = NULL;
  FpiByteWriter writer;
  gboolean written = TRUE;

  fpi_byte_writer_init(&writer);
  // Dummy
  written &= fpi_byte_writer_put_uint8(&writer, 1);
  g_assert(written);

  g_autoptr(TlsRecord) record = g_new0(TlsRecord, 1);

  record->type = SSL3_RT_CHANGE_CIPHER_SPEC;
  record->length = fpi_byte_writer_get_pos(&writer);
  record->fragment = fpi_byte_writer_reset_and_get_data(&writer);
  asprintf(&record->repr, "ChangeCipherSpec()");

  tls_session_send(self, record, &local_error);
  if (local_error) g_propagate_error(error, local_error);

  if (!tls_session_flush_content_buffer(self, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

static gboolean tls_session_receive_handshake(TlsSession *self, Handshake *msg,
                                              GError **error)
{
  GError *local_error = NULL;
  FpiByteReader reader;
  gboolean read_ok = TRUE;

  // Update hash
  // BROKEN The windows driver only updates it when the message isn't "Finished"
  if (msg->msg_type != SSL3_MT_FINISHED)
  {
    gboolean written = TRUE;

    written &=
        fpi_byte_writer_put_uint8(&self->handshake_buffer, msg->msg_type);
    written &=
        fpi_byte_writer_put_uint24_be(&self->handshake_buffer, msg->length);
    written &= fpi_byte_writer_put_data(&self->handshake_buffer, msg->body,
                                        msg->length);

    g_assert(written);
  }

  fpi_byte_reader_init(&reader, msg->body, msg->length);
  switch (msg->msg_type)
  {
    case SSL3_MT_SERVER_HELLO:
    {
      if (self->handshake_phase != CLIENT_HELLO_SENT) g_assert(FALSE);

      guint16 proto_ver, cipher_suite;  // extension_len;
      guint8 session_id_len, compr_method, extensions_num = 0;
      g_autofree guint8 *session_id = NULL;
      g_autofree guint8 *random = NULL;

      read_ok &= fpi_byte_reader_get_uint16_be(&reader, &proto_ver);
      read_ok &= fpi_byte_reader_dup_data(&reader, RANDOM_SIZE, &random);
      read_ok &= fpi_byte_reader_get_uint8(&reader, &session_id_len);
      g_assert(session_id_len <= MAX_SESSION_ID_SIZE);
      read_ok &= fpi_byte_reader_dup_data(&reader, session_id_len, &session_id);
      read_ok &= fpi_byte_reader_get_uint16_be(&reader, &cipher_suite);
      read_ok &= fpi_byte_reader_get_uint8(&reader, &compr_method);

      // while (fpi_byte_reader_get_remaining(&reader) > 0) {
      //   read_ok &= fpi_byte_reader_get_uint16_be(&reader2,
      //   &extension_len); read_ok &= fpi_byte_reader_skip(&reader2,
      //   extension_len); extensions_num++;
      // }

      g_assert(read_ok);

      g_autofree gchar *ses_id_str = bin2hex(session_id, session_id_len);
      g_autofree gchar *rand_str = bin2hex(random, RANDOM_SIZE);

#if DEBUG_SSL
      fp_dbg(
          "<- HandshakeMessage(type=0x%02x, "
          "content=ServerHello(ver=0x%04x, rand=%s, ses_id='%s', "
          "cipher_suite=0x%04x, compr_method=0x%02x, extensions[%d])",
          msg->msg_type, proto_ver, rand_str, ses_id_str, cipher_suite,
          compr_method, extensions_num);
#endif

      // Store server random
      memcpy(self->server_random, random, RANDOM_SIZE);

      // The windows driver does implement (broken) resuming, but it is never
      // used
      self->session_id = g_memdup2(session_id, session_id_len);

      // TODO:
      // raise
      // data.TlsAlertException(data.TlsAlertDescription.HANDSHAKE_FAILURE)
      g_assert(cipher_suite == TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384);
      self->pending_cs = cipher_suite;

      // As we don't advertise any compression methods (NOT STANDARD COMPLIANT),
      // just fallback to the null one
      // TODO:
      // raise
      // data.TlsAlertException(data.TlsAlertDescription.HANDSHAKE_FAILURE)
      g_assert(compr_method == 0x00);

      // At this point, the cipher suite takes over handshake negotiation
      fp_dbg("Starting cipher suite handshake...");
      self->handshake_phase = SUITE_HANDSHAKE;
      break;
    }
    case SSL3_MT_CERTIFICATE_REQUEST:
    {
      if (self->handshake_phase != SUITE_HANDSHAKE) g_assert(FALSE);

      if (self->cert_request != 0) g_assert(FALSE);

      guint8 certs_num;
      guint8 certificate_type;

      read_ok &= fpi_byte_reader_get_uint8(&reader, &certs_num);
      g_assert(certs_num == 1);

      read_ok &= fpi_byte_reader_get_uint8(&reader, &certificate_type);

      // Some garbage bytes
      fpi_byte_reader_skip(&reader, 2);
      g_assert(read_ok);

#if DEBUG_SSL
      fp_dbg(
          "<- HandshakeMessage(type=0x%02x, "
          "content=CertificateRequest(types=[%d]))",
          msg->msg_type, certificate_type);
#endif

      self->cert_request = certificate_type;
      break;
    }
    case SSL3_MT_SERVER_DONE:
    {
      if (self->handshake_phase != SUITE_HANDSHAKE) g_assert(FALSE);

#if DEBUG_SSL
      fp_dbg("<- HandshakeMessage(type=0x%02x, content=ServerHelloDone())",
             msg->msg_type);
#endif

      // End the suite handshake
      // The server must have requested a certificate
      // TODO:
      // if ( self.cert_request == None
      //     or not data.TlsCertificateType.ECDSA_SIGN in
      //     self.cert_request.cert_types
      // ):
      //     raise
      //     data.TlsAlertException(data.TlsAlertDescription.HANDSHAKE_FAILURE)
      g_assert(self->cert_request == ECDSA_SIGN);
      if (!tls_session_send_certificate(self, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      // Create ephemeral ECC key pair & send it to the server
      g_autoptr(EVP_PKEY) eph_key = EVP_EC_gen("prime256v1");

      guint8 *eph_pubkey = NULL;
      gsize eph_pubkey_len =
          EVP_PKEY_get1_encoded_public_key(eph_key, &eph_pubkey);
      if (eph_pubkey_len == 0) g_assert_not_reached();

      if (!tls_session_send_client_key_exchange(self, eph_pubkey,
                                                eph_pubkey_len, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      gsize hdata_len = fpi_byte_writer_get_pos(&self->handshake_buffer);
      g_autofree guint8 *hdata =
          fpi_byte_writer_reset_and_get_data(&self->handshake_buffer);
      fpi_byte_writer_init(&self->handshake_buffer);
      fpi_byte_writer_put_data(&self->handshake_buffer, hdata, hdata_len);

      g_autoptr(EVP_MD_CTX) ctx = EVP_MD_CTX_new();
      g_autofree guint8 *signature = NULL;
      gsize signature_len;

      if (ctx == NULL ||
          !EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL,
                              self->pairing_data->remote_key) ||
          !EVP_DigestSignUpdate(ctx, hdata, hdata_len) ||
          !EVP_DigestSignFinal(ctx, NULL, &signature_len))
        g_assert_not_reached();

      signature = g_malloc(signature_len);

      if (!EVP_DigestSignFinal(ctx, signature, &signature_len))
        g_assert_not_reached();

      if (!tls_session_send_certificate_verify(self, signature, signature_len,
                                               &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      // Calculate premaster secret
      g_autoptr(EVP_PKEY_CTX) pctx = EVP_PKEY_CTX_new(eph_key, NULL);
      g_autofree guint8 *premaster_secret = NULL;
      gsize premaster_secret_len;

      if (ctx == NULL || !EVP_PKEY_derive_init(pctx) ||
          !EVP_PKEY_derive_set_peer(pctx,
                                    self->pairing_data->remote_cert.pub_key) ||
          !EVP_PKEY_derive(pctx, NULL, &premaster_secret_len))
        g_assert_not_reached();

      premaster_secret = g_malloc(premaster_secret_len);

      if (!EVP_PKEY_derive(pctx, premaster_secret, &premaster_secret_len))
        g_assert_not_reached();

      fp_dbg("Cipher suite handshake ended");

      // Calculate master secret
      uint8_t rnd[2 * RANDOM_SIZE];
      memcpy(rnd, self->client_random, RANDOM_SIZE);
      memcpy(rnd + RANDOM_SIZE, self->server_random, RANDOM_SIZE);

      gchar *label = "master secret";
      if (!tls_prf(premaster_secret, premaster_secret_len, label, strlen(label),
                   rnd, sizeof(rnd), self->master_secret, MASTER_SECRET_SIZE,
                   self->hash_algo, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      guint8 key_block[MAX_KEY_BLOCK_SIZE];
      label = "key expansion";
      if (!tls_prf(self->master_secret, MASTER_SECRET_SIZE, label,
                   strlen(label), rnd, sizeof(rnd), key_block,
                   MAX_KEY_BLOCK_SIZE, self->hash_algo, NULL))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      // Send "Change Cipher Spec" & "Finished" messages
      if (!tls_session_send_change_cipher_spec(self, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      guint8 *key_block_ptr = key_block;

      self->encr_key_len = 32;
      self->encr_key = g_memdup2(key_block_ptr, 32);
      key_block_ptr += 32;

      self->decr_key_len = 32;
      self->decr_key = g_memdup2(key_block_ptr, 32);
      key_block_ptr += 32;

      self->encr_iv_len = 4;
      self->encr_iv = g_memdup2(key_block_ptr, 4);
      key_block_ptr += 4;

      self->decr_iv_len = 4;
      self->decr_iv = g_memdup2(key_block_ptr, 4);

      self->client_cs = self->pending_cs;

      label = "client finished";
      guint8 verify_data[VERIFY_DATA_SIZE];

      gsize digest_size = EVP_MD_size(EVP_sha256());
      guint8 digest[digest_size];

      tls_session_handshake_hash(self, digest, &digest_size);

      if (!tls_prf(self->master_secret, MASTER_SECRET_SIZE, label,
                   strlen(label), digest, digest_size, verify_data,
                   VERIFY_DATA_SIZE, self->hash_algo, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      if (!tls_session_send_finished(self, verify_data, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      // Wait for the server's "Finished" message
      self->handshake_phase = SERVER_DONE;
      break;
    }
    case SSL3_MT_FINISHED:
    {
      if (self->handshake_phase != SERVER_DONE) g_assert(FALSE);

      // Server must have sent "Change Cipher Spec"
      if (self->server_cs != self->client_cs) g_assert(FALSE);

      g_autofree guint8 *remote_verify_data;
      read_ok &= fpi_byte_reader_dup_data(&reader, 12, &remote_verify_data);
      g_assert(read_ok);

#if DEBUG_SSL
      g_autofree gchar *verify_data_str = bin2hex(remote_verify_data, 12);
      fp_dbg(
          "<- HandshakeMessage(type=0x%02x, content=Finished(verify_data=%s))",
          msg->msg_type, verify_data_str);
#endif

      // Handle verify data
      gchar *label = "server finished";
      guint8 verify_data[VERIFY_DATA_SIZE];

      gsize digest_size = EVP_MD_size(EVP_sha256());
      guint8 digest[digest_size];

      tls_session_handshake_hash(self, digest, &digest_size);

      if (!tls_prf(self->master_secret, MASTER_SECRET_SIZE, label,
                   strlen(label), digest, digest_size, verify_data,
                   VERIFY_DATA_SIZE, self->hash_algo, &local_error))
      {
        g_propagate_error(error, local_error);
        return FALSE;
      }

      if (memcmp(verify_data, remote_verify_data, VERIFY_DATA_SIZE) != 0)
      {
        fp_err("Verify data do not match");
        g_assert_not_reached();
        // if ver_data != msg.verify_data:
        //     raise
        //     data.TlsAlertException(data.TlsAlertDescription.DECRYPT_ERROR)
      }

      // The handshake is now done
      self->handshake_phase = FINISHED;
    }
  }

  return TRUE;
}

static gboolean tls_session_receive(TlsSession *self, TlsRecord *record,
                                    GError **error)
{
  GError *local_error = NULL;
  FpiByteReader reader;
  gboolean read_ok = TRUE;

  // We don't implement fragmentation, as the windows driver also doesn't
  fpi_byte_reader_init(&reader, record->fragment, record->length);
  while ((fpi_byte_reader_get_remaining(&reader) != 0) && read_ok)
  {
    // Handle fragment content
    switch (record->type)
    {
      case SSL3_RT_CHANGE_CIPHER_SPEC:
      {
        guint8 dummy;

        read_ok &= fpi_byte_reader_get_uint8(&reader, &dummy);

        g_assert(read_ok && dummy == 1);

#if DEBUG_SSL
        fp_dbg("<- ChangeCipherSpec");
#endif

        // Switch encryption algorithms
        self->server_cs = self->pending_cs;
        self->pending_cs = TLS_NULL_WITH_NULL_NULL;
        break;
      }
      case SSL3_RT_ALERT:
      {
        guint8 alert_level, alert_description;

        read_ok &= fpi_byte_reader_get_uint8(&reader, &alert_level);
        read_ok &= fpi_byte_reader_get_uint8(&reader, &alert_description);

        if (!read_ok)
        {
          fp_err("Invalid length of received TLS alert message");
          g_assert(FALSE);
        }

#if DEBUG_SSL
        fp_dbg("<- TlsAlert(level=\"%s\", description=\"%s\")",
               alert_level == SSL3_AL_WARNING ? "warning" : "fatal",
               SSL_alert_desc_string_long(alert_description));
#endif

        // Handle the alert
        if (alert_description == SSL3_AD_CLOSE_NOTIFY)
        {
          if (self->send_closed)
          {
            fp_dbg("Remote confirmed session close");
          }
          else
          {
            if (!tls_session_close(self, &local_error))
            {
              g_propagate_error(error, local_error);
              return FALSE;
            }
            fp_err("Remote closed session unexpectedly");
            g_assert(FALSE);
          }

          self->recv_closed = TRUE;
          return TRUE;
        }

        if (alert_level == SSL3_AL_FATAL)
        {
          if (!tls_session_close(self, &local_error))
          {
            g_propagate_error(error, local_error);
            return FALSE;
          }
          g_assert_not_reached();
          // TODO: raise data.TlsAlertException(alert.descr, True)
        }
        break;
      }
      case SSL3_RT_HANDSHAKE:
      {
        g_autoptr(Handshake) msg = g_new0(Handshake, 1);

        read_ok &= fpi_byte_reader_get_uint8(&reader, &msg->msg_type);
        read_ok &= fpi_byte_reader_get_uint24_be(&reader, &msg->length);
        read_ok &= fpi_byte_reader_dup_data(&reader, msg->length, &msg->body);

        g_assert(read_ok);

        if (!tls_session_receive_handshake(self, msg, &local_error))
        {
          g_propagate_error(error, local_error);
          return FALSE;
        }
        break;
      }
      case SSL3_RT_APPLICATION_DATA:
      {
        gsize app_data_size;
        const guint8 *app_data;

        app_data_size = fpi_byte_reader_get_remaining(&reader);
        read_ok = fpi_byte_reader_get_data(&reader, app_data_size, &app_data);

#if DEBUG_SSL
        g_autofree gchar *app_data_str = bin2hex(app_data, app_data_size);
        fp_dbg("<- ApplicationData(data=%s)", app_data_str);
#endif

        fpi_byte_writer_put_data(&self->application_data, app_data,
                                 app_data_size);

        break;
      }
      default:
        fp_err("Got unimplemented record type: %d", record->type);
        // TODO:
        // raise data.TlsAlertException(data.TlsAlertDescription.DECODE_ERROR)
        g_assert(FALSE);
        break;
    }
  }

  return TRUE;
}

gboolean tls_session_receive_ciphertext(TlsSession *self, guint8 *data,
                                        gsize size, GError **error)
{
  GError *local_error = NULL;
  FpiByteReader reader;
  gboolean read_ok = TRUE;

  g_assert(!self->recv_closed);

  fpi_byte_reader_init(&reader, data, size);
  while ((fpi_byte_reader_get_remaining(&reader) != 0) && read_ok)
  {
    guint16 version;
    guint8 content_type;
    guint16 cfrag_size;
    gsize pfrag_size;
    g_autofree guint8 *cfrag = NULL;
    g_autofree guint8 *pfrag = NULL;

    read_ok &= fpi_byte_reader_get_uint8(&reader, &content_type);
    read_ok &= fpi_byte_reader_get_uint16_be(&reader, &version);
    read_ok &= fpi_byte_reader_get_uint16_be(&reader, &cfrag_size);
    read_ok &= fpi_byte_reader_dup_data(&reader, cfrag_size, &cfrag);

    g_assert(read_ok);

    // TODO: if ptext.proto_ver != data.TlsProtocolVersion.current:
    // raise data.TlsAlertException(data.TlsAlertDescription.PROTOCOL_VERSION)
    g_assert(version == self->version);

    // Read TlsCiphertext and convert to plaintext
    tls_session_decrypt(self, content_type, version, cfrag, cfrag_size, &pfrag,
                        &pfrag_size, &local_error);

    TlsRecord plaintext = {.type = content_type,
                           .version = self->version,
                           .length = pfrag_size,
                           .fragment = pfrag};

    if (!tls_session_receive(self, &plaintext, &local_error))
    {
      g_propagate_error(error, local_error);
      return FALSE;
    }
  }

  return TRUE;
}

gboolean tls_session_has_data(TlsSession *self)
{
  return fpi_byte_writer_get_pos(&self->send_buffer) > 0 ||
         fpi_byte_writer_get_pos(&self->content_buffer) > 0;
}

gboolean tls_session_establish(TlsSession *self, GError **error)
{
  GError *local_error = NULL;
  g_assert(self->handshake_phase == HANDSHAKE_BEGIN);

  fp_dbg("Starting TLS handshake...");

  if (!tls_session_send_client_hello(self, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  self->handshake_phase = CLIENT_HELLO_SENT;

  return TRUE;
}

TlsSession *tls_session_new(void)
{
  TlsSession *new = g_new0(TlsSession, 1);

  // The windows client always sends a 7 byte session id
  new->session_id_size = 7;
  new->session_id = g_malloc0(7);

  return new;
}

gboolean tls_session_init(TlsSession *self, SensorPairingData *pairing_data,
                          GError **error)
{
  RAND_bytes(self->client_random, RANDOM_SIZE);

  self->version = TLS1_2_VERSION;

  self->suites_size = 2;
  self->suites = g_malloc(2);
  FP_WRITE_UINT16_BE(self->suites, TLS_ECDH_ECDSA_WITH_AES_256_GCM_SHA384);
  self->hash_algo = SN_sha384;

  self->server_cs = TLS_NULL_WITH_NULL_NULL;
  self->client_cs = TLS_NULL_WITH_NULL_NULL;
  self->pending_cs = TLS_NULL_WITH_NULL_NULL;

  self->pairing_data = pairing_data;

  fpi_byte_writer_init(&self->handshake_buffer);
  fpi_byte_writer_init(&self->send_buffer);
  fpi_byte_writer_init(&self->content_buffer);
  fpi_byte_writer_init(&self->application_data);

  return TRUE;
}

static gboolean tls_session_send_application_data(TlsSession *self,
                                                  guint8 *data, gsize length,
                                                  GError **error)
{
  GError *local_error = NULL;

  g_autoptr(TlsRecord) record = g_new0(TlsRecord, 1);

  record->type = SSL3_RT_APPLICATION_DATA;
  record->length = length;
  record->fragment = g_memdup2(data, length);
  g_autofree gchar *data_str = bin2hex(data, length);
  asprintf(&record->repr, "ApplicationData(data=%s)", data_str);

  if (!tls_session_send(self, record, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

gboolean tls_session_wrap(TlsSession *self, guint8 *pdata, gsize pdata_size,
                          guint8 **cdata, gsize *cdata_size, GError **error)
{
  GError *local_error = NULL;
  // g_assert(self->established);

  // Send application data message
  if (!tls_session_send_application_data(self, pdata, pdata_size, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  if (!tls_session_flush_send_buffer(self, cdata, cdata_size, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  return TRUE;
}

gboolean tls_session_unwrap(TlsSession *self, guint8 *cdata, gsize cdata_size,
                            guint8 **pdata, gsize *pdata_size, GError **error)
{
  GError *local_error = NULL;
  // g_assert(self->established);

  // "Receive" data
  if (!tls_session_receive_ciphertext(self, cdata, cdata_size, &local_error))
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  *pdata_size = fpi_byte_writer_get_pos(&self->application_data);
  *pdata = fpi_byte_writer_reset_and_get_data(&self->application_data);
  fpi_byte_writer_init(&self->application_data);

  return TRUE;
}

static gboolean generate_hs_priv_key(EVP_PKEY *privkey, GError **error)
{
  gboolean ret = TRUE;

  guint8 secret[] = {0x71, 0x7c, 0xd7, 0x2d, 0x09, 0x62, 0xbc, 0x4a,
                     0x28, 0x46, 0x13, 0x8d, 0xbb, 0x2c, 0x24, 0x19};
  guint8 seed[] = {0x25, 0x12, 0xa7, 0x64, 0x07, 0x06, 0x5f, 0x38, 0x38,
                   0x46, 0x13, 0x9d, 0x4b, 0xec, 0x20, 0x33, 0xaa, 0xaa};
  char *label = "HS_KEY_PAIR_GEN";

  g_autofree guint8 *privkey_k = NULL;
  const gsize privkey_k_size = ECC_KEY_SIZE;
  if (!tls_prf(secret, sizeof(secret), label, strlen(label), seed, sizeof(seed),
               privkey_k, privkey_k_size, SN_sha256, error))
  {
    ret = FALSE;
    goto error;
  }

  /* output is in little-endian, OpenSSL expects big-endian
   * TODO: verify that openssl does not reverse array */
  reverse_array(privkey_k, privkey_k_size);

  // FIXME: import private key

  // NOTE: expected result
  /* HS_KEY_PAIR_GEN result, then converted to big-endian as expected by OpenSSL
   */
  /* guint8 k[ECC_KEY_SIZE] = {0xe8, 0xa2, 0xa2, 0xb6, 0x65, 0x62, 0x54, 0xd6,
                            0xac, 0xb0, 0xef, 0x47, 0x9c, 0xae, 0x41, 0x40,
                            0xc7, 0xe8, 0xe2, 0x60, 0xdb, 0x3f, 0x64, 0x2e,
                            0x35, 0xd4, 0x09, 0x9c, 0x01, 0xb3, 0x6a, 0x86};
  guint8 x[ECC_KEY_SIZE] = {0x89, 0xe5, 0x41, 0x30, 0x0c, 0xcf, 0x1a, 0x03,
                            0xe6, 0x25, 0xc4, 0x3d, 0xf7, 0x25, 0xc5, 0x95,
                            0x78, 0x7a, 0x71, 0xcb, 0x03, 0x5b, 0x4b, 0x7c,
                            0x06, 0xd3, 0x51, 0x71, 0x42, 0x2e, 0x50, 0x57};
  guint8 y[ECC_KEY_SIZE] = {0xeb, 0x05, 0x00, 0x8f, 0x22, 0xaa, 0x2b, 0xc6,
                            0xfe, 0x0b, 0xf9, 0x08, 0x03, 0xa0, 0xe7, 0x3a,
                            0x2e, 0xb2, 0x8c, 0xfd, 0x0c, 0x72, 0xa5, 0xf6,
                            0x73, 0x35, 0xc0, 0x61, 0x22, 0x6e, 0xff, 0xec}; */

error:
  return ret;
}

gboolean create_host_certificate(SensorPairingData pairing_data,
                                 guint8 *host_certificate, GError **error)
{
  gboolean ret = TRUE;

  // FIXME: load public key from self->pairing_data.private_key and export to
  // certificate

  /* as the size of public key x and y is up to 68 we need to zero the unused
   * bytes */
  FpiByteWriter writer;
  fpi_byte_writer_init_with_data(&writer, host_certificate, CERTIFICATE_SIZE,
                                 FALSE);

  gboolean written = TRUE;
  written &= fpi_byte_writer_put_uint16_le(&writer, CERTIFICATE_MAGIC);
  written &= fpi_byte_writer_put_uint16_le(&writer, CERTIFICATE_CURVE);

  // FIXME: hs_privkey needs to be loaded and used to sign the generated
  // certificate NOTE: everything is written in little endian
  // reverse_array(x.data, x.size);
  // written &= fpi_byte_writer_put_data(&writer, x.data, x.size);
  // /* add zeros as padding */
  // written &= fpi_byte_writer_fill(&writer, 0, 68 - x.size);
  // reverse_array(y.data, y.size);
  // written &= fpi_byte_writer_put_data(&writer, y.data, y.size);
  // /* add zeros as padding */
  // written &= fpi_byte_writer_fill(&writer, 0, 68 - y.size);
  // /* add padding */
  // written &= fpi_byte_writer_put_uint8(&writer, 0);
  // /* put certificate type */
  // written &= fpi_byte_writer_put_uint8(&writer, 0);

  // g_assert(fpi_byte_writer_get_pos(&writer) ==
  //          CERTIFICATE_DATA_SIZE);
  // g_assert(written);

  // if (!generate_hs_priv_key(&hs_privkey, error)) {
  //    ret = FALSE;
  //    goto error;
  // }
  // hs_privkey_initialized = TRUE;

  // gnutls_datum_t to_sign = {.data = host_certificate,
  //                           .size = CERTIFICATE_SIZE_WITHOUT_SIGNATURE};
  // GNUTLS_CHECK(gnutls_privkey_sign_data(hs_privkey, GNUTLS_DIG_SHA256, 0,
  //                                       &to_sign, &signature));
  // g_assert(signature.size <= SIGNATURE_SIZE);

  // written &= fpi_byte_writer_put_uint16_le(&writer, signature.size);

  // written &= fpi_byte_writer_put_data(&writer, signature.data,
  // signature.size);
  // /* add zeros as padding */
  // written &= fpi_byte_writer_fill(&writer, 0, SIGNATURE_SIZE -
  // signature.size); WRITTEN_CHECK(written);
  // g_assert(fpi_byte_writer_get_pos(&writer) == CERTIFICATE_SIZE);

error:
  return ret;
}

void free_pairing_data(SensorPairingData *pairing_data)
{
  g_free(pairing_data->remote_cert_raw);
  g_free(pairing_data->remote_cert.sign);
  g_free(pairing_data->remote_cert.x);
  g_free(pairing_data->remote_cert.y);
  g_free(pairing_data->client_cert_raw);
  g_free(pairing_data->client_cert.sign);
  g_free(pairing_data->client_cert.x);
  g_free(pairing_data->client_cert.y);
  EVP_PKEY_free(pairing_data->client_cert.pub_key);
  EVP_PKEY_free(pairing_data->remote_cert.pub_key);
}
