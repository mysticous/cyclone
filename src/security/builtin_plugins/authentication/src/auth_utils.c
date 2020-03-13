/*
 * Copyright(c) 2006 to 2019 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

#include <assert.h>
#include <string.h>
#include <openssl/bn.h>
#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#if OPENSSL_VERSION_NUMBER >= 0x1000200fL
#define AUTH_INCLUDE_EC
#include <openssl/ec.h>
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
#define AUTH_INCLUDE_DH_ACCESSORS
#endif
#else
#error "OpenSSL version is not supported"
#endif
#include "dds/ddsrt/time.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/filesystem.h"
#include "dds/ddsrt/retcode.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/atomics.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/io.h"
#include "dds/security/dds_security_api_defs.h"
#include "dds/security/core/dds_security_utils.h"
#include "auth_utils.h"

/* There is a problem when compiling on windows w.r.t. X509_NAME.
 * The windows api already defines the type X509_NAME which
 * conficts with some openssl versions. The workaround is to
 * undef the openssl X509_NAME
 */
#ifdef _WIN32
#undef X509_NAME
#endif

#define MAX_TRUSTED_CA 100

char *get_openssl_error_message(void)
{
  char *msg, *buf = NULL;
  size_t len;
  BIO *bio = BIO_new(BIO_s_mem());
  if (!bio)
    return ddsrt_strdup("BIO_new failed");

  ERR_print_errors(bio);
  len = (size_t)BIO_get_mem_data(bio, &buf);
  msg = ddsrt_malloc(len + 1);
  memcpy(msg, buf, len);
  msg[len] = '\0';
  BIO_free(bio);
  return msg;
}

char *get_certificate_subject_name(X509 *cert, DDS_Security_SecurityException *ex)
{
  X509_NAME *name;
  assert(cert);
  if (!(name = X509_get_subject_name(cert)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "X509_get_subject_name failed : ");
    return NULL;
  }
  char *subject_openssl = X509_NAME_oneline(name, NULL, 0);
  char *subject = ddsrt_strdup(subject_openssl);
  OPENSSL_free(subject_openssl);
  return subject;
}

dds_time_t get_certificate_expiry(const X509 *cert)
{
  assert(cert);
  ASN1_TIME *asn1 = X509_get_notAfter(cert);
  if (asn1 != NULL)
  {
    int days, seconds;
    if (ASN1_TIME_diff(&days, &seconds, NULL, asn1) == 1)
    {
      static const dds_duration_t secs_in_day = 86400;
      const dds_time_t now = dds_time();
      const int64_t max_valid_days_to_wait = (INT64_MAX - now) / DDS_NSECS_IN_SEC / secs_in_day;
      if (days < max_valid_days_to_wait)
      {
        dds_duration_t delta = ((dds_duration_t)seconds + ((dds_duration_t)days * secs_in_day)) * DDS_NSECS_IN_SEC;
        return now + delta;
      }
      return DDS_NEVER;
    }
  }
  return DDS_TIME_INVALID;
}

DDS_Security_ValidationResult_t get_subject_name_DER_encoded(const X509 *cert, unsigned char **buffer, size_t *size, DDS_Security_SecurityException *ex)
{
  unsigned char *tmp = NULL;
  int32_t sz;
  X509_NAME *name;

  assert(cert);
  assert(buffer);
  assert(size);

  *size = 0;
  if (!(name = X509_get_subject_name((X509 *)cert)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "X509_get_subject_name failed : ");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if ((sz = i2d_X509_NAME(name, &tmp)) <= 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "i2d_X509_NAME failed : ");
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  *size = (size_t)sz;
  *buffer = ddsrt_malloc(*size);
  memcpy(*buffer, tmp, *size);
  OPENSSL_free(tmp);
  return DDS_SECURITY_VALIDATION_OK;
}

static DDS_Security_ValidationResult_t check_key_type_and_size(EVP_PKEY *key, int isPrivate, DDS_Security_SecurityException *ex)
{
  const char *sub = isPrivate ? "private key" : "certificate";
  assert(key);
  switch (EVP_PKEY_id(key))
  {
  case EVP_PKEY_RSA:
    if (EVP_PKEY_bits(key) != 2048)
    {
      DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "RSA %s has unsupported key size (%d)", sub, EVP_PKEY_bits(key));
      return DDS_SECURITY_VALIDATION_FAILED;
    }
    if (isPrivate)
    {
      RSA *rsaKey = EVP_PKEY_get1_RSA(key);
      if (rsaKey && RSA_check_key(rsaKey) != 1)
      {
        DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "RSA key not correct : ");
        return DDS_SECURITY_VALIDATION_FAILED;
      }
      RSA_free(rsaKey);
    }
    return DDS_SECURITY_VALIDATION_OK;

  case EVP_PKEY_EC:
    if (EVP_PKEY_bits(key) != 256)
    {
      DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "EC %s has unsupported key size (%d)", sub, EVP_PKEY_bits(key));
      return DDS_SECURITY_VALIDATION_FAILED;
    }
    EC_KEY *ecKey = EVP_PKEY_get1_EC_KEY(key);
    if (ecKey && EC_KEY_check_key(ecKey) != 1)
    {
      DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "EC key not correct : ");
      return DDS_SECURITY_VALIDATION_FAILED;
    }
    EC_KEY_free(ecKey);
    return DDS_SECURITY_VALIDATION_OK;

  default:
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "%s has not supported type", sub);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
}

static DDS_Security_ValidationResult_t check_certificate_type_and_size(X509 *cert, DDS_Security_SecurityException *ex)
{
  assert(cert);
  EVP_PKEY *pkey = X509_get_pubkey(cert);
  if (!pkey)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "X509_get_pubkey failed");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  DDS_Security_ValidationResult_t result = check_key_type_and_size(pkey, false, ex);
  EVP_PKEY_free(pkey);
  return result;
}

DDS_Security_ValidationResult_t check_certificate_expiry(const X509 *cert, DDS_Security_SecurityException *ex)
{
  assert(cert);
  if (X509_cmp_current_time(X509_get_notBefore(cert)) == 0)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_CERT_STARTDATE_IN_FUTURE_CODE, DDS_SECURITY_VALIDATION_FAILED, DDS_SECURITY_ERR_CERT_STARTDATE_IN_FUTURE_MESSAGE);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (X509_cmp_current_time(X509_get_notAfter(cert)) == 0)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_CERT_EXPIRED_CODE, DDS_SECURITY_VALIDATION_FAILED, DDS_SECURITY_ERR_CERT_EXPIRED_MESSAGE);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  return DDS_SECURITY_VALIDATION_OK;
}

DDS_Security_ValidationResult_t load_X509_certificate_from_data(const char *data, int len, X509 **x509Cert, DDS_Security_SecurityException *ex)
{
  BIO *bio;
  assert(data);
  assert(len >= 0);
  assert(x509Cert);

  if (!(bio = BIO_new_mem_buf((void *)data, len)))
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "BIO_new_mem_buf failed");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (!(*x509Cert = PEM_read_bio_X509(bio, NULL, NULL, NULL)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to parse certificate: ");
    BIO_free(bio);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (get_authentication_algo_kind(*x509Cert) == AUTH_ALGO_KIND_UNKNOWN)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_CERT_AUTH_ALGO_KIND_UNKNOWN_CODE, DDS_SECURITY_VALIDATION_FAILED, DDS_SECURITY_ERR_CERT_AUTH_ALGO_KIND_UNKNOWN_MESSAGE);
    X509_free(*x509Cert);
    BIO_free(bio);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  BIO_free(bio);
  return DDS_SECURITY_VALIDATION_OK;
}

DDS_Security_ValidationResult_t load_X509_certificate_from_file(const char *filename, X509 **x509Cert, DDS_Security_SecurityException *ex)
{
  assert(filename);
  assert(x509Cert);

  DDSRT_WARNING_MSVC_OFF(4996);
  FILE *file_ptr = fopen(filename, "r");
  DDSRT_WARNING_MSVC_ON(4996);

  if (file_ptr == NULL)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_INVALID_FILE_PATH_CODE, DDS_SECURITY_VALIDATION_FAILED, DDS_SECURITY_ERR_INVALID_FILE_PATH_MESSAGE, filename);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (!(*x509Cert = PEM_read_X509(file_ptr, NULL, NULL, NULL)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to parse certificate: ");
    (void)fclose(file_ptr);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (get_authentication_algo_kind(*x509Cert) == AUTH_ALGO_KIND_UNKNOWN)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_CERT_AUTH_ALGO_KIND_UNKNOWN_CODE, DDS_SECURITY_VALIDATION_FAILED, DDS_SECURITY_ERR_CERT_AUTH_ALGO_KIND_UNKNOWN_MESSAGE);
    X509_free(*x509Cert);
    (void)fclose(file_ptr);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  (void)fclose(file_ptr);
  return DDS_SECURITY_VALIDATION_OK;
}

static DDS_Security_ValidationResult_t load_private_key_from_data(const char *data, const char *password, EVP_PKEY **privateKey, DDS_Security_SecurityException *ex)
{
  BIO *bio;
  assert(data);
  assert(privateKey);

  if (!(bio = BIO_new_mem_buf((void *)data, -1)))
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "BIO_new_mem_buf failed");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (!(*privateKey = PEM_read_bio_PrivateKey(bio, NULL, NULL, (void *)(password ? password : ""))))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to parse private key: ");
    BIO_free(bio);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  BIO_free(bio);
  return DDS_SECURITY_VALIDATION_OK;
}

static DDS_Security_ValidationResult_t load_private_key_from_file(const char *filepath, const char *password, EVP_PKEY **privateKey, DDS_Security_SecurityException *ex)
{
  FILE *file_ptr;
  assert(filepath);
  assert(privateKey);

  DDSRT_WARNING_MSVC_OFF(4996);
  file_ptr = fopen(filepath, "r");
  DDSRT_WARNING_MSVC_ON(4996);
  if (file_ptr == NULL)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_INVALID_FILE_PATH_CODE, DDS_SECURITY_VALIDATION_FAILED, DDS_SECURITY_ERR_INVALID_FILE_PATH_MESSAGE, filepath);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (!(*privateKey = PEM_read_PrivateKey(file_ptr, NULL, NULL, (void *)(password ? password : ""))))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to parse certificate: ");
    (void)fclose(file_ptr);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  (void)fclose(file_ptr);
  return DDS_SECURITY_VALIDATION_OK;
}

/*
 * Gets the URI string (as referred in DDS Security spec) and returns the URI type
 * data: data part of the URI. Typically It contains different format according to URI type.
 */
AuthConfItemPrefix_t get_conf_item_type(const char *str, char **data)
{
  const char *pf_f = "file:", *pf_d = "data:,", *pf_p = "pkcs11:";
  size_t len_pf_f = strlen(pf_f);
  size_t len_pf_d = strlen(pf_d);
  size_t len_pf_p = strlen(pf_p);
  char *ptr;
  assert(str);
  assert(data);

  ptr = ddssec_strchrs(str, " \t", false);

  if (strncmp(ptr, pf_f, len_pf_f) == 0)
  {
    if (strncmp(&(ptr[len_pf_f]), "//", 2) == 0)
      *data = ddsrt_strdup(&(ptr[len_pf_f + 2]));
    else
      *data = ddsrt_strdup(&(ptr[len_pf_f]));
    return AUTH_CONF_ITEM_PREFIX_FILE;
  }
  if (strncmp(ptr, pf_d, len_pf_d) == 0)
  {
    *data = ddsrt_strdup(&(ptr[len_pf_d]));
    return AUTH_CONF_ITEM_PREFIX_DATA;
  }
  if (strncmp(ptr, pf_p, len_pf_p) == 0)
  {
    *data = ddsrt_strdup(&(ptr[len_pf_p]));
    return AUTH_CONF_ITEM_PREFIX_PKCS11;
  }

  return AUTH_CONF_ITEM_PREFIX_UNKNOWN;
}

DDS_Security_ValidationResult_t load_X509_certificate(const char *data, X509 **x509Cert, DDS_Security_SecurityException *ex)
{
  DDS_Security_ValidationResult_t result;
  char *contents = NULL;
  assert(data);
  assert(x509Cert);

  switch (get_conf_item_type(data, &contents))
  {
  case AUTH_CONF_ITEM_PREFIX_FILE:
    result = load_X509_certificate_from_file(contents, x509Cert, ex);
    break;
  case AUTH_CONF_ITEM_PREFIX_DATA:
    result = load_X509_certificate_from_data(contents, (int)strlen(contents), x509Cert, ex);
    break;
  case AUTH_CONF_ITEM_PREFIX_PKCS11:
    result = DDS_SECURITY_VALIDATION_FAILED;
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "Certificate pkcs11 format currently not supported:\n%s", data);
    break;
  default:
    result = DDS_SECURITY_VALIDATION_FAILED;
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "Specified certificate has wrong format:\n%s", data);
    break;
  }
  ddsrt_free(contents);

  if (result == DDS_SECURITY_VALIDATION_OK)
  {
    if (check_certificate_type_and_size(*x509Cert, ex) != DDS_SECURITY_VALIDATION_OK ||
        check_certificate_expiry(*x509Cert, ex) != DDS_SECURITY_VALIDATION_OK)
    {
      result = DDS_SECURITY_VALIDATION_FAILED;
      X509_free(*x509Cert);
    }
  }
  return result;
}

DDS_Security_ValidationResult_t load_X509_private_key(const char *data, const char *password, EVP_PKEY **privateKey, DDS_Security_SecurityException *ex)
{
  DDS_Security_ValidationResult_t result;
  char *contents = NULL;
  assert(data);
  assert(privateKey);

  switch (get_conf_item_type(data, &contents))
  {
  case AUTH_CONF_ITEM_PREFIX_FILE:
    result = load_private_key_from_file(contents, password, privateKey, ex);
    break;
  case AUTH_CONF_ITEM_PREFIX_DATA:
    result = load_private_key_from_data(contents, password, privateKey, ex);
    break;
  case AUTH_CONF_ITEM_PREFIX_PKCS11:
    result = DDS_SECURITY_VALIDATION_FAILED;
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "PrivateKey pkcs11 format currently not supported:\n%s", data);
    break;
  default:
    result = DDS_SECURITY_VALIDATION_FAILED;
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "Specified PrivateKey has wrong format:\n%s", data);
    break;
  }
  ddsrt_free(contents);

  if (result == DDS_SECURITY_VALIDATION_OK && ((result = check_key_type_and_size(*privateKey, true, ex)) != DDS_SECURITY_VALIDATION_OK))
    EVP_PKEY_free(*privateKey);

  return result;
}

DDS_Security_ValidationResult_t verify_certificate(X509 *identityCert, X509 *identityCa, DDS_Security_SecurityException *ex)
{
  DDS_Security_ValidationResult_t result = DDS_SECURITY_VALIDATION_OK;
  X509_STORE_CTX *ctx;
  X509_STORE *store;
  assert(identityCert);
  assert(identityCa);

  /* Currently only a self signed indentiyCa is supported. Verification of against a certificate chain is not yet supported */
  /* Verification of the certificate expiry using a CRL is not yet supported */

  if (!(store = X509_STORE_new()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "X509_STORE_new failed : ");
    goto err_store_new;
  }
  if (X509_STORE_add_cert(store, identityCa) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "X509_STORE_add_cert failed : ");
    goto err_add_cert;
  }
  if (!(ctx = X509_STORE_CTX_new()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "X509_STORE_CTX_new failed : ");
    goto err_ctx_new;
  }
  if (X509_STORE_CTX_init(ctx, store, identityCert, NULL) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "X509_STORE_CTX_init failed : ");
    goto err_ctx_init;
  }
  int r = X509_verify_cert(ctx);
  if (r != 1)
  {
    const char *msg = X509_verify_cert_error_string(X509_STORE_CTX_get_error(ctx));
    char *subject = NULL;
    result = DDS_SECURITY_VALIDATION_FAILED;
    subject = get_certificate_subject_name(identityCert, NULL);
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, (int)result, "Certificate not valid: error: %s; subject: %s", msg, subject ? subject : "[not found]");
    ddsrt_free(subject);
  }

err_ctx_init:
  X509_STORE_CTX_free(ctx);
err_ctx_new:
err_add_cert:
  X509_STORE_free(store);
err_store_new:
  return result;
}

AuthenticationAlgoKind_t get_authentication_algo_kind(X509 *cert)
{
  AuthenticationAlgoKind_t kind = AUTH_ALGO_KIND_UNKNOWN;
  assert(cert);
  EVP_PKEY *pkey = X509_get_pubkey(cert);
  if (pkey)
  {
    switch (EVP_PKEY_id(pkey))
    {
    case EVP_PKEY_RSA:
      if (EVP_PKEY_bits(pkey) == 2048)
        kind = AUTH_ALGO_KIND_RSA_2048;
      break;
    case EVP_PKEY_EC:
      if (EVP_PKEY_bits(pkey) == 256)
        kind = AUTH_ALGO_KIND_EC_PRIME256V1;
      break;
    }
    EVP_PKEY_free(pkey);
  }
  return kind;
}

AuthenticationChallenge * generate_challenge(DDS_Security_SecurityException *ex)
{
  AuthenticationChallenge *result = ddsrt_malloc(sizeof(*result));
  if (RAND_bytes(result->value, sizeof(result->value)) < 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to generate a 256 bit random number ");
    ddsrt_free(result);
    return NULL;
  }
  return result;
}

DDS_Security_ValidationResult_t get_certificate_contents(X509 *cert, unsigned char **data, uint32_t *size, DDS_Security_SecurityException *ex)
{
  BIO *bio = NULL;
  char *ptr;
  if ((bio = BIO_new(BIO_s_mem())) == NULL)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "BIO_new_mem_buf failed");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (!PEM_write_bio_X509(bio, cert))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "PEM_write_bio_X509 failed: ");
    BIO_free(bio);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  size_t sz = (size_t)BIO_get_mem_data(bio, &ptr);
  *data = ddsrt_malloc(sz + 1);
  memcpy(*data, ptr, sz);
  (*data)[sz] = '\0';
  *size = (uint32_t)sz;
  BIO_free(bio);
  return DDS_SECURITY_VALIDATION_OK;
}

static DDS_Security_ValidationResult_t get_rsa_dh_parameters(EVP_PKEY **params, DDS_Security_SecurityException *ex)
{
  DH *dh = NULL;
  *params = NULL;
  if ((*params = EVP_PKEY_new()) == NULL)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate DH generation parameters: ");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if ((dh = DH_get_2048_256()) == NULL)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate DH parameter using DH_get_2048_256: ");
    DH_free(dh);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (EVP_PKEY_set1_DH(*params, dh) <= 0)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to set DH generation parameters using EVP_PKEY_set1_DH: ");
    EVP_PKEY_free(*params);
    DH_free(dh);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  DH_free(dh);
  return DDS_SECURITY_VALIDATION_OK;
}

static DDS_Security_ValidationResult_t get_ec_dh_parameters(EVP_PKEY **params, DDS_Security_SecurityException *ex)
{
  EVP_PKEY_CTX *pctx = NULL;
  if ((pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL)) == NULL)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate DH parameter context: ");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (EVP_PKEY_paramgen_init(pctx) <= 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to initialize DH generation context: ");
    EVP_PKEY_CTX_free(pctx);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to set DH generation parameter generation method: ");
    EVP_PKEY_CTX_free(pctx);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (EVP_PKEY_paramgen(pctx, params) <= 0)
  {
    char *msg = get_openssl_error_message();
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to generate DH parameters: ");
    ddsrt_free(msg);
    EVP_PKEY_CTX_free(pctx);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  EVP_PKEY_CTX_free(pctx);
  return DDS_SECURITY_VALIDATION_OK;
}

DDS_Security_ValidationResult_t generate_dh_keys(EVP_PKEY **dhkey, AuthenticationAlgoKind_t authKind, DDS_Security_SecurityException *ex)
{
  EVP_PKEY *params = NULL;
  EVP_PKEY_CTX *kctx = NULL;
  *dhkey = NULL;
  switch (authKind)
  {
  case AUTH_ALGO_KIND_RSA_2048:
    if (get_rsa_dh_parameters(&params, ex) != DDS_SECURITY_VALIDATION_OK)
      goto failed;
    break;
  case AUTH_ALGO_KIND_EC_PRIME256V1:
    if (get_ec_dh_parameters(&params, ex) != DDS_SECURITY_VALIDATION_OK)
      goto failed;
    break;
  default:
    assert(0);
    goto failed;
  }

  if ((kctx = EVP_PKEY_CTX_new(params, NULL)) == NULL)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate DH generation context: ");
    goto failed_params;
  }
  if (EVP_PKEY_keygen_init(kctx) <= 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to initialize DH generation context: ");
    goto failed_kctx;
  }
  if (EVP_PKEY_keygen(kctx, dhkey) <= 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to generate DH key pair: ");
    goto failed_kctx;
  }
  EVP_PKEY_CTX_free(kctx);
  EVP_PKEY_free(params);
  return DDS_SECURITY_VALIDATION_OK;

failed_kctx:
  EVP_PKEY_CTX_free(kctx);
failed_params:
  EVP_PKEY_free(params);
failed:
  return DDS_SECURITY_VALIDATION_FAILED;
}

static const BIGNUM *dh_get_public_key(DH *dhkey)
{
#ifdef AUTH_INCLUDE_DH_ACCESSORS
  const BIGNUM *pubkey, *privkey;
  DH_get0_key(dhkey, &pubkey, &privkey);
  return pubkey;
#else
  return dhkey->pub_key;
#endif
}

static int dh_set_public_key(DH *dhkey, BIGNUM *pubkey)
{
#ifdef AUTH_INCLUDE_DH_ACCESSORS
  return DH_set0_key(dhkey, pubkey, NULL);
#else
  dhkey->pub_key = pubkey;
#endif
  return 1;
}

static DDS_Security_ValidationResult_t dh_public_key_to_oct_modp(EVP_PKEY *pkey, unsigned char **buffer, uint32_t *length, DDS_Security_SecurityException *ex)
{
  DH *dhkey;
  ASN1_INTEGER *asn1int;
  *buffer = NULL;
  if (!(dhkey = EVP_PKEY_get1_DH(pkey)))
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to get DH key from PKEY: ");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  if (!(asn1int = BN_to_ASN1_INTEGER(dh_get_public_key(dhkey), NULL)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to convert DH key to ASN1 integer: ");
    DH_free(dhkey);
    return DDS_SECURITY_VALIDATION_FAILED;
  }
  *length = (uint32_t)i2d_ASN1_INTEGER(asn1int, buffer);
  ASN1_INTEGER_free(asn1int);
  DH_free(dhkey);
  return DDS_SECURITY_VALIDATION_OK;
}

static DDS_Security_ValidationResult_t dh_public_key_to_oct_ecdh(EVP_PKEY *pkey, unsigned char **buffer, uint32_t *length, DDS_Security_SecurityException *ex)
{
  EC_KEY *eckey;
  const EC_GROUP *group;
  const EC_POINT *point;
  size_t sz;

  if (!(eckey = EVP_PKEY_get1_EC_KEY(pkey)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to get EC key from PKEY: ");
    goto failed;
  }
  if (!(point = EC_KEY_get0_public_key(eckey)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to get public key from ECKEY: ");
    goto failed;
  }
  if (!(group = EC_KEY_get0_group(eckey)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to get group from ECKEY: ");
    goto failed;
  }
  if ((sz = EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, NULL, 0, NULL)) == 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to serialize public EC key: ");
    goto failed;
  }
  *buffer = ddsrt_malloc(sz);
  if ((*length = (uint32_t)EC_POINT_point2oct(group, point, POINT_CONVERSION_UNCOMPRESSED, *buffer, sz, NULL)) == 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to serialize public EC key: ");
    ddsrt_free(*buffer);
    goto failed;
  }
  EC_KEY_free(eckey);
  return DDS_SECURITY_VALIDATION_OK;

failed:
  EC_KEY_free(eckey);
  return DDS_SECURITY_VALIDATION_FAILED;
}

DDS_Security_ValidationResult_t dh_public_key_to_oct(EVP_PKEY *pkey, AuthenticationAlgoKind_t algo, unsigned char **buffer, uint32_t *length, DDS_Security_SecurityException *ex)
{
  assert(pkey);
  assert(buffer);
  assert(length);
  switch (algo)
  {
  case AUTH_ALGO_KIND_RSA_2048:
    return dh_public_key_to_oct_modp(pkey, buffer, length, ex);
  case AUTH_ALGO_KIND_EC_PRIME256V1:
    return dh_public_key_to_oct_ecdh(pkey, buffer, length, ex);
  default:
    assert(0);
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Invalid key algorithm specified");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
}

static DDS_Security_ValidationResult_t dh_oct_to_public_key_modp(EVP_PKEY **pkey, const unsigned char *keystr, uint32_t size, DDS_Security_SecurityException *ex)
{
  DH *dhkey;
  ASN1_INTEGER *asn1int;
  BIGNUM *pubkey;

  if (!(*pkey = EVP_PKEY_new()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to convert octet sequence to ASN1 integer: ");
    goto fail_alloc_pkey;
  }
  if (!(asn1int = d2i_ASN1_INTEGER(NULL, (const unsigned char **)&keystr, size)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to convert octet sequence to ASN1 integer: ");
    goto fail_get_asn1int;
  }
  if (!(pubkey = ASN1_INTEGER_to_BN(asn1int, NULL)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to convert ASN1 integer to BIGNUM: ");
    goto fail_get_pubkey;
  }

  dhkey = DH_get_2048_256();
  if (dh_set_public_key(dhkey, pubkey) == 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to set DH public key: ");
    goto fail_get_pubkey;
  }
  if (EVP_PKEY_set1_DH(*pkey, dhkey) == 0)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to convert DH to PKEY: ");
    DH_free(dhkey);
    goto fail_get_pubkey;
  }
  ASN1_INTEGER_free(asn1int);
  DH_free(dhkey);
  return DDS_SECURITY_VALIDATION_OK;

fail_get_pubkey:
  ASN1_INTEGER_free(asn1int);
fail_get_asn1int:
  EVP_PKEY_free(*pkey);
fail_alloc_pkey:
  return DDS_SECURITY_VALIDATION_FAILED;
}

static DDS_Security_ValidationResult_t dh_oct_to_public_key_ecdh(EVP_PKEY **pkey, const unsigned char *keystr, uint32_t size, DDS_Security_SecurityException *ex)
{
  EC_KEY *eckey;
  EC_GROUP *group;
  EC_POINT *point;
  if (!(group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate EC group: ");
    goto fail_alloc_group;
  }
  if (!(point = EC_POINT_new(group)))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate EC point: ");
    goto fail_alloc_point;
  }
  if (EC_POINT_oct2point(group, point, keystr, size, NULL) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to deserialize EC public key to EC point: ");
    goto fail_oct2point;
  }
  if (!(eckey = EC_KEY_new()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate EC KEY: ");
    goto fail_alloc_eckey;
  }
  if (EC_KEY_set_group(eckey, group) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to convert octet sequence to ASN1 integer: ");
    goto fail_eckey_set_group;
  }
  if (EC_KEY_set_public_key(eckey, point) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to set EC public key: ");
    goto fail_eckey_set_pubkey;
  }
  if (!(*pkey = EVP_PKEY_new()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to allocate EVP key: ");
    goto fail_alloc_pkey;
  }
  if (EVP_PKEY_set1_EC_KEY(*pkey, eckey) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to set EVP key to EC public key: ");
    goto fail_pkey_set_eckey;
  }
  EC_KEY_free(eckey);
  EC_POINT_free(point);
  EC_GROUP_free(group);
  return DDS_SECURITY_VALIDATION_OK;

fail_pkey_set_eckey:
  EVP_PKEY_free(*pkey);
fail_alloc_pkey:
fail_eckey_set_pubkey:
fail_eckey_set_group:
  EC_KEY_free(eckey);
fail_alloc_eckey:
fail_oct2point:
  EC_POINT_free(point);
fail_alloc_point:
  EC_GROUP_free(group);
fail_alloc_group:
  return DDS_SECURITY_VALIDATION_FAILED;
}

DDS_Security_ValidationResult_t dh_oct_to_public_key(EVP_PKEY **data, AuthenticationAlgoKind_t algo, const unsigned char *str, uint32_t size, DDS_Security_SecurityException *ex)
{
  assert(data);
  assert(str);
  switch (algo)
  {
  case AUTH_ALGO_KIND_RSA_2048:
    return dh_oct_to_public_key_modp(data, str, size, ex);
  case AUTH_ALGO_KIND_EC_PRIME256V1:
    return dh_oct_to_public_key_ecdh(data, str, size, ex);
  default:
    assert(0);
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Invalid key algorithm specified");
    return DDS_SECURITY_VALIDATION_FAILED;
  }
}

char *string_from_data(const unsigned char *data, uint32_t size)
{
  char *str = NULL;
  if (size > 0 && data)
  {
    str = ddsrt_malloc(size + 1);
    memcpy(str, data, size);
    str[size] = '\0';
  }
  return str;
}

void free_ca_list_contents(X509Seq *ca_list)
{
  unsigned i;
  if (ca_list->buffer != NULL && ca_list->length > 0)
  {
    for (i = 0; i < ca_list->length; ++i)
      X509_free(ca_list->buffer[i]);
    ddsrt_free(ca_list->buffer);
  }
  ca_list->buffer = NULL;
  ca_list->length = 0;
}

DDS_Security_ValidationResult_t get_trusted_ca_list(const char *trusted_ca_dir, X509Seq *ca_list, DDS_Security_SecurityException *ex)
{
  dds_return_t ret;
  ddsrt_dir_handle_t d_descr;
  struct ddsrt_dirent d_entry;
  struct ddsrt_stat status;
  char *trusted_ca_dir_normalized;

  X509 *ca_buffer_array[MAX_TRUSTED_CA];
  unsigned ca_buffer_array_size = 0;
  trusted_ca_dir_normalized = ddsrt_file_normalize(trusted_ca_dir);

  ret = ddsrt_opendir(trusted_ca_dir_normalized, &d_descr);
  ddsrt_free(trusted_ca_dir_normalized);
  if (ret != DDS_RETCODE_OK || ca_buffer_array_size >= MAX_TRUSTED_CA)
  {
    DDS_Security_Exception_set(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_INVALID_TRUSTED_CA_DIR_CODE, 0, DDS_SECURITY_ERR_INVALID_TRUSTED_CA_DIR_MESSAGE);
    return DDS_SECURITY_VALIDATION_FAILED;
  }

  while ((ret = ddsrt_readdir(d_descr, &d_entry)) == DDS_RETCODE_OK)
  {
    char *full_file_path;
    ddsrt_asprintf(&full_file_path, "%s%s%s", trusted_ca_dir, ddsrt_file_sep(), d_entry.d_name);
    if (ddsrt_stat(full_file_path, &status) == DDS_RETCODE_OK)
    {
      if (strcmp(d_entry.d_name, ".") != 0 && strcmp(d_entry.d_name, "..") != 0)
      {
        char *filename = ddsrt_file_normalize(full_file_path);
        if (filename)
        {
          X509 *identityCA;
          if (load_X509_certificate_from_file(filename, &identityCA, ex) == DDS_SECURITY_VALIDATION_OK)
          {
            ca_buffer_array[ca_buffer_array_size] = identityCA;
            ca_buffer_array_size++;
          }
          else
            DDS_Security_Exception_reset(ex);
          ddsrt_free(filename);
        }
      }
    }
    ddsrt_free(full_file_path);
  }

  ddsrt_closedir(d_descr);
  free_ca_list_contents(ca_list);

  if (ca_buffer_array_size > 0)
  {
    ca_list->buffer = ddsrt_malloc(ca_buffer_array_size * sizeof(X509 *));
    for (unsigned i = 0; i < ca_buffer_array_size; ++i)
      ca_list->buffer[i] = ca_buffer_array[i];
  }
  ca_list->length = ca_buffer_array_size;
  return DDS_SECURITY_VALIDATION_OK;
}

DDS_Security_ValidationResult_t create_asymmetrical_signature(EVP_PKEY *pkey, const unsigned char *data, const size_t dataLen,
    unsigned char **signature, size_t *signatureLen, DDS_Security_SecurityException *ex)
{
  EVP_MD_CTX *mdctx = NULL;
  EVP_PKEY_CTX *kctx = NULL;

  if (!(mdctx = EVP_MD_CTX_create()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to create signing context: ");
    goto err_create_ctx;
  }
  if (EVP_DigestSignInit(mdctx, &kctx, EVP_sha256(), NULL, pkey) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to initialize signing context: ");
    goto err_sign;
  }
  if (EVP_PKEY_id(pkey) == EVP_PKEY_RSA)
  {
    if (EVP_PKEY_CTX_set_rsa_padding(kctx, RSA_PKCS1_PSS_PADDING) < 1)
    {
      DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to initialize signing context: ");
      goto err_sign;
    }
  }
  if (EVP_DigestSignUpdate(mdctx, data, dataLen) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to update signing context: ");
    goto err_sign;
  }
  if (EVP_DigestSignFinal(mdctx, NULL, signatureLen) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to finalize signing context: ");
    goto err_sign;
  }
  *signature = ddsrt_malloc(sizeof(unsigned char) * (*signatureLen));
  if (EVP_DigestSignFinal(mdctx, *signature, signatureLen) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to finalize signing context: ");
    ddsrt_free(*signature);
    goto err_sign;
  }
  EVP_MD_CTX_destroy(mdctx);
  return DDS_SECURITY_VALIDATION_OK;

err_sign:
  EVP_MD_CTX_destroy(mdctx);
err_create_ctx:
  return DDS_SECURITY_VALIDATION_FAILED;
}

DDS_Security_ValidationResult_t validate_asymmetrical_signature(EVP_PKEY *pkey, const unsigned char *data, const size_t dataLen,
    const unsigned char *signature, const size_t signatureLen, DDS_Security_SecurityException *ex)
{
  EVP_MD_CTX *mdctx = NULL;
  EVP_PKEY_CTX *kctx = NULL;
  if (!(mdctx = EVP_MD_CTX_create()))
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to create verify context: ");
    goto err_create_ctx;
  }
  if (EVP_DigestVerifyInit(mdctx, &kctx, EVP_sha256(), NULL, pkey) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to initialize verify context: ");
    goto err_verify;
  }
  if (EVP_PKEY_id(pkey) == EVP_PKEY_RSA)
  {
    if (EVP_PKEY_CTX_set_rsa_padding(kctx, RSA_PKCS1_PSS_PADDING) < 1)
    {
      DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to initialize signing context: ");
      goto err_verify;
    }
  }
  if (EVP_DigestVerifyUpdate(mdctx, data, dataLen) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to update verify context: ");
    goto err_verify;
  }
  if (EVP_DigestVerifyFinal(mdctx, signature, signatureLen) != 1)
  {
    DDS_Security_Exception_set_with_openssl_error(ex, DDS_AUTH_PLUGIN_CONTEXT, DDS_SECURITY_ERR_UNDEFINED_CODE, DDS_SECURITY_VALIDATION_FAILED, "Failed to finalize verify context: ");
    goto err_verify;
  }
  EVP_MD_CTX_destroy(mdctx);
  return DDS_SECURITY_VALIDATION_OK;

err_verify:
  EVP_MD_CTX_destroy(mdctx);
err_create_ctx:
  return DDS_SECURITY_VALIDATION_FAILED;
}
