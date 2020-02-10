/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#ifdef DDSI_INCLUDE_SECURITY

#include <string.h>
#include <stdarg.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/misc.h"
#include "dds/ddsrt/avl.h"

#include "dds/ddsi/ddsi_domaingv.h"
#include "dds/ddsi/q_unused.h"
#include "dds/ddsi/q_bswap.h"
#include "dds/ddsi/q_radmin.h"
#include "dds/ddsi/q_misc.h"
#include "dds/ddsi/ddsi_entity_index.h"
#include "dds/ddsi/ddsi_security_msg.h"
#include "dds/ddsi/ddsi_security_omg.h"
#include "dds/ddsi/ddsi_security_util.h"
#include "dds/ddsi/ddsi_security_exchange.h"
#include "dds/ddsi/ddsi_handshake.h"
#include "dds/ddsi/ddsi_sertopic.h"
#include "dds/ddsi/q_config.h"
#include "dds/ddsi/q_log.h"
#include "dds/ddsrt/sync.h"
#include "dds/security/dds_security_api.h"
#include "dds/security/core/dds_security_utils.h"
#include "dds/security/core/dds_security_plugins.h"
#include "dds/ddsrt/hopscotch.h"

#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/q_xevent.h"
#include "dds/ddsi/ddsi_plist.h"
#include "dds/ddsi/sysdeps.h"

#define AUTH_NAME "Authentication"
#define AC_NAME "Access Control"
#define CRYPTO_NAME "Cryptographic"

#define EXCEPTION_LOG(sc,e,cat, ...) \
  q_omg_log_exception(sc->logcfg, cat, e, __FILE__, __LINE__, DDS_FUNCTION, __VA_ARGS__)

#define EXCEPTION_ERROR(s, e, ...)     EXCEPTION_LOG(s, e, DDS_LC_ERROR, __VA_ARGS__)
#define EXCEPTION_WARNING(s, e, ...)   EXCEPTION_LOG(s, e, DDS_LC_WARNING, __VA_ARGS__)


#define SECURITY_ATTR_IS_VALID(attr)                                      \
    ((attr) & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID)

/* Security attributes are compatible ... */
#define SECURITY_ATTR_COMPATIBLE(attr_a, attr_b, is_valid_flag)           \
(                                                                         \
    /* ... if masks are equal ... */                                      \
    (attr_a == attr_b)                                                    \
    ||                                                                    \
    /* ... or if either of the masks is not valid ... */                  \
    (((attr_a & is_valid_flag) == 0) || ((attr_b & is_valid_flag) == 0))  \
)

/* Security information are compatible ... */
#define SECURITY_INFO_COMPATIBLE(info_a, info_b, is_valid_flag)           \
(                                                                         \
    /* ... if plugin attributes are compatible ... */                     \
    SECURITY_ATTR_COMPATIBLE(info_a.plugin_security_attributes,           \
                             info_b.plugin_security_attributes,           \
                             is_valid_flag)                               \
    &&                                                                    \
    /* ... and spec attributes are compatible ... */                      \
    SECURITY_ATTR_COMPATIBLE(info_a.security_attributes,                  \
                             info_b.security_attributes,                  \
                             is_valid_flag)                               \
)

/* Security information indicates clear data ... */
#define SECURITY_INFO_CLEAR(info, is_valid_flag)                          \
(                                                                         \
    /* ... if no flag was set (ignoring the is_valid flag) ... */         \
    (info.security_attributes & (~is_valid_flag)) == 0                    \
)

#define SECURITY_INFO_IS_RTPS_PROTECTED(info)                                                 \
(                                                                                             \
    (info.security_attributes & NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_VALID         ) && \
    (info.security_attributes & NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_RTPS_PROTECTED)    \
)

#define SECURITY_INFO_IS_WRITE_PROTECTED(info)                                              \
(                                                                                           \
    (info.security_attributes & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID          ) && \
    (info.security_attributes & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_WRITE_PROTECTED)    \
)

#define SECURITY_INFO_IS_READ_PROTECTED(info)                                               \
(                                                                                           \
    (info.security_attributes & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID          ) && \
    (info.security_attributes & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_READ_PROTECTED )    \
)

#define SECURITY_INFO_IS_RTPS_PROTECTED(info)                                                 \
(                                                                                             \
    (info.security_attributes & NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_VALID         ) && \
    (info.security_attributes & NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_RTPS_PROTECTED)    \
)

static bool endpoint_is_DCPSParticipantSecure (const ddsi_guid_t *guid)
{
  return ((guid->entityid.u == NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER) ||
          (guid->entityid.u == NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_READER));
}

static bool endpoint_is_DCPSPublicationsSecure (const ddsi_guid_t *guid)
{
  return ((guid->entityid.u == NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_WRITER) ||
          (guid->entityid.u == NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_READER));
}

static bool endpoint_is_DCPSSubscriptionsSecure (const ddsi_guid_t *guid)
{
  return ((guid->entityid.u == NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_WRITER) ||
          (guid->entityid.u == NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_READER));
}

static bool endpoint_is_DCPSParticipantStatelessMessage (const ddsi_guid_t *guid)
{
  return ((guid->entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_STATELESS_MESSAGE_WRITER) ||
          (guid->entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_STATELESS_MESSAGE_READER));
}

static bool endpoint_is_DCPSParticipantMessageSecure (const ddsi_guid_t *guid)
{
  return ((guid->entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_WRITER) ||
          (guid->entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_READER));
}

static bool endpoint_is_DCPSParticipantVolatileMessageSecure (const ddsi_guid_t *guid)
{
  return ((guid->entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER) ||
          (guid->entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_READER));
}

struct dds_security_context {
  dds_security_plugin auth_plugin;
  dds_security_plugin ac_plugin;
  dds_security_plugin crypto_plugin;

  dds_security_authentication *authentication_context;
  dds_security_cryptography *crypto_context;
  dds_security_access_control *access_control_context;
  ddsrt_mutex_t omg_security_lock;
  uint32_t next_plugin_id;

  const struct ddsrt_log_cfg *logcfg;
};

typedef struct dds_security_context dds_security_context;

struct guid_pair {
  ddsi_guid_t src;
  ddsi_guid_t dst;
};

struct security_entity_match {
  ddsrt_avl_node_t avlnode;
  struct guid_pair guids;
  bool matched;
  int64_t crypto_handle;
  DDS_Security_ParticipantCryptoTokenSeq *tokens;
};

struct dds_security_match_index {
  ddsrt_mutex_t lock;
  ddsrt_avl_tree_t matches;
};

struct pp_proxypp_match {
  ddsrt_avl_node_t avlnode;
  ddsi_guid_t proxypp_guid;
  DDS_Security_ParticipantCryptoHandle proxypp_crypto_handle;
};

struct proxypp_pp_match {
  ddsrt_avl_node_t avlnode;
  ddsi_guid_t pp_guid;
  DDS_Security_IdentityHandle proxypp_identity_handle;
  DDS_Security_ParticipantCryptoHandle pp_crypto_handle;
  DDS_Security_ParticipantCryptoHandle proxypp_crypto_handle;
  DDS_Security_PermissionsHandle permissions_handle;
  DDS_Security_SharedSecretHandle shared_secret;
};

struct participant_sec_attributes {
  DDS_Security_ParticipantSecurityAttributes attr;
  DDS_Security_ParticipantCryptoHandle crypto_handle;
  bool plugin_attr;
  ddsrt_mutex_t lock;
  ddsrt_avl_ctree_t proxy_participants;
};

struct proxy_participant_sec_attributes {
  struct dds_security_context *sc;
  ddsrt_mutex_t lock;
  ddsrt_avl_tree_t participants;
};

struct writer_sec_attributes {
  DDS_Security_EndpointSecurityAttributes attr;
  DDS_Security_DatawriterCryptoHandle crypto_handle;
  bool plugin_attr;
};

struct reader_sec_attributes {
  DDS_Security_EndpointSecurityAttributes attr;
  DDS_Security_DatareaderCryptoHandle crypto_handle;
  bool plugin_attr;
};

struct cleanup_participant_crypto {
  ddsrt_avl_node_t avlnode;
  struct ddsi_domaingv *gv;
  struct dds_security_context *sc;
  ddsi_guid_t guid;
  struct participant_sec_attributes *sec_attr;
};

struct dds_security_garbage {
  ddsrt_mutex_t lock;
  ddsrt_avl_tree_t pp_crypto_list;
};

static int compare_guid(const void *va, const void *vb);
static int compare_crypto_handle (const void *va, const void *vb);
static int compare_guid_pair(const void *va, const void *vb);
static int compare_security_attribute(const void *va, const void *vb);

const ddsrt_avl_ctreedef_t pp_proxypp_treedef =
    DDSRT_AVL_CTREEDEF_INITIALIZER (offsetof (struct pp_proxypp_match, avlnode), offsetof (struct pp_proxypp_match, proxypp_guid), compare_guid, 0);
const ddsrt_avl_treedef_t proxypp_pp_treedef =
  DDSRT_AVL_TREEDEF_INITIALIZER (offsetof (struct proxypp_pp_match, avlnode), offsetof (struct proxypp_pp_match, pp_crypto_handle), compare_crypto_handle, 0);
const ddsrt_avl_treedef_t entity_match_treedef =
  DDSRT_AVL_TREEDEF_INITIALIZER (offsetof (struct security_entity_match, avlnode), offsetof (struct security_entity_match, guids), compare_guid_pair, 0);
const ddsrt_avl_treedef_t participant_garbage_treedef =
    DDSRT_AVL_TREEDEF_INITIALIZER_INDKEY (offsetof (struct cleanup_participant_crypto, avlnode), offsetof (struct cleanup_participant_crypto, sec_attr), compare_security_attribute, 0);

static int compare_crypto_handle (const void *va, const void *vb)
{
  const DDS_Security_ParticipantCryptoHandle *ha = va;
  const DDS_Security_ParticipantCryptoHandle *hb = vb;

  return ((*ha > *hb) ? 1 : (*ha < *hb) ?  -1 : 0);
}

static int guid_compare (const ddsi_guid_t *guid1, const ddsi_guid_t *guid2)
{
  return memcmp (guid1, guid2, sizeof (ddsi_guid_t));
}

static int compare_guid(const void *va, const void *vb)
{
  const ddsi_guid_t *ga = va;
  const ddsi_guid_t *gb = vb;

  return guid_compare(ga, gb);
}

static int compare_guid_pair(const void *va, const void *vb)
{
  const struct guid_pair *na = va;
  const struct guid_pair *nb = vb;
  int r;

  if ((r = guid_compare(&na->src, &nb->src)) == 0)
    r = guid_compare(&na->dst, &nb->dst);
  return r;
}

static int compare_security_attribute(const void *va, const void *vb)
{
  const struct participant_sec_attributes *sa = va;
  const struct participant_sec_attributes *sb = vb;

  return compare_crypto_handle(&sa->crypto_handle, &sb->crypto_handle);
}

static struct dds_security_context * q_omg_security_get_secure_context(const struct participant *pp)
{
  if (pp && pp->e.gv->security_context && q_omg_is_security_loaded(pp->e.gv->security_context))
    return pp->e.gv->security_context;
  return NULL;
}

struct dds_security_authentication *q_omg_participant_get_authentication(const struct participant *pp)
{
  if (pp && pp->e.gv->security_context && q_omg_is_security_loaded(pp->e.gv->security_context))
    return pp->e.gv->security_context->authentication_context;
  return NULL;
}

static struct dds_security_context * q_omg_security_get_secure_context_from_proxypp(const struct proxy_participant *proxypp)
{
  if (proxypp && proxypp->e.gv->security_context && q_omg_is_security_loaded(proxypp->e.gv->security_context))
    return proxypp->e.gv->security_context;
  return NULL;
}

void q_omg_log_exception(const struct ddsrt_log_cfg *lc, uint32_t cat, DDS_Security_SecurityException *exception, const char *file, uint32_t line, const char *func, const char *fmt, ...)
{
  char logbuffer[512];
  va_list ap;
  int l;

  va_start (ap, fmt);
  l = vsnprintf(logbuffer, sizeof(logbuffer), fmt, ap);
  va_end (ap);
  if ((size_t) l >= sizeof(logbuffer))
  {
    logbuffer[sizeof(logbuffer)-1] = '\0';
  }
  dds_log_cfg(lc, cat, file, line, func, "%s: %s(code: %d)\n", logbuffer, exception->message ? exception->message : "",  exception->code);
  DDS_Security_Exception_reset(exception);
}

static struct security_entity_match * entity_match_new(const ddsi_guid_t *src, const ddsi_guid_t *dst)
{
  struct security_entity_match *match;

  match = ddsrt_malloc(sizeof(*match));
  match->guids.src = *src;
  match->guids.dst = *dst;
  match->matched = false;
  match->crypto_handle = 0;
  match->tokens = NULL;

  return match;
}

static void entity_match_free(struct security_entity_match *match)
{
  if (match) {
    if (match->tokens)
      DDS_Security_ParticipantCryptoTokenSeq_free(match->tokens);
    ddsrt_free(match);
  }
}

static struct security_entity_match * find_entity_match_locked(struct dds_security_match_index *list, const ddsi_guid_t *src, const ddsi_guid_t *dst)
{
  struct guid_pair guids;

  guids.src = *src;
  guids.dst = *dst;

  return ddsrt_avl_lookup(&entity_match_treedef, &list->matches, &guids);
}

static struct security_entity_match * find_or_create_entity_match(struct dds_security_match_index *list, const ddsi_guid_t *src, const ddsi_guid_t *dst)
{
  struct security_entity_match *match;

  ddsrt_mutex_lock(&list->lock);
  match = find_entity_match_locked(list, src, dst);
  if (!match)
  {
    match = entity_match_new(src, dst);
    ddsrt_avl_insert(&entity_match_treedef, &list->matches, match);
  }
  ddsrt_mutex_unlock(&list->lock);

  return match;
}

static struct security_entity_match * remove_entity_match(struct dds_security_match_index *list, const ddsi_guid_t *src, const ddsi_guid_t *dst)
{
  struct security_entity_match *match;
  struct guid_pair guids;
  ddsrt_avl_dpath_t path;

  guids.src = *src;
  guids.dst = *dst;

  ddsrt_mutex_lock(&list->lock);
  match = ddsrt_avl_lookup_dpath(&entity_match_treedef, &list->matches, &guids, &path);
  if (match)
    ddsrt_avl_delete_dpath(&entity_match_treedef, &list->matches, match, &path);
  ddsrt_mutex_unlock(&list->lock);

  return match;
}

static struct dds_security_match_index * security_match_index_new(void)
{
  struct dds_security_match_index *list;

  list  = ddsrt_malloc (sizeof(*list));
  ddsrt_mutex_init (&list->lock);
  ddsrt_avl_init (&entity_match_treedef, &list->matches);
  return list;
}

static void entity_match_free_wrapper(void *arg)
{
  struct security_entity_match *match = arg;
  entity_match_free(match);
}

static void security_match_index_free(struct dds_security_match_index *list)
{
  if (list)
  {
    ddsrt_avl_free (&entity_match_treedef, &list->matches, entity_match_free_wrapper);
    ddsrt_mutex_destroy(&list->lock);
    ddsrt_free(list);
  }
}

static struct pp_proxypp_match * pp_proxypp_match_new(struct proxy_participant *proxypp, DDS_Security_ParticipantCryptoHandle proxypp_crypto_handle)
{
  struct pp_proxypp_match *pm;

  pm = ddsrt_malloc(sizeof(*pm));
  pm->proxypp_guid = proxypp->e.guid;
  pm->proxypp_crypto_handle = proxypp_crypto_handle;

  return pm;
}

static void pp_proxypp_match_free(struct dds_security_context *sc, struct pp_proxypp_match *pm)
{
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;

  if (pm->proxypp_crypto_handle != DDS_SECURITY_HANDLE_NIL)
  {
    if (!sc->crypto_context->crypto_key_factory->unregister_participant(sc->crypto_context->crypto_key_factory, pm->proxypp_crypto_handle, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return permissions handle");
  }
  ddsrt_free(pm);
}

static struct proxypp_pp_match * proxypp_pp_match_new(struct participant *pp, DDS_Security_IdentityHandle identity_handle, DDS_Security_ParticipantCryptoHandle proxypp_crypto_handle, DDS_Security_PermissionsHandle permissions_hdl, DDS_Security_SharedSecretHandle shared_secret)
{
  struct proxypp_pp_match *pm;

  pm = ddsrt_malloc(sizeof(*pm));
  pm->pp_guid = pp->e.guid;
  pm->proxypp_identity_handle = identity_handle;
  pm->pp_crypto_handle = pp->sec_attr->crypto_handle;
  pm->proxypp_crypto_handle = proxypp_crypto_handle;
  pm->permissions_handle = permissions_hdl;
  pm->shared_secret = shared_secret;

  return pm;
}

static void proxypp_pp_match_free(struct dds_security_context *sc, struct proxypp_pp_match *pm)
{
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;

  if (pm->proxypp_crypto_handle != DDS_SECURITY_HANDLE_NIL)
  {
    if (!sc->crypto_context->crypto_key_factory->unregister_participant(sc->crypto_context->crypto_key_factory, pm->proxypp_crypto_handle, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return permissions handle");
  }
  if (pm->permissions_handle != DDS_SECURITY_HANDLE_NIL)
  {
    if (!sc->access_control_context->return_permissions_handle(sc->access_control_context, pm->permissions_handle, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return permissions handle");
  }
  if (pm->proxypp_identity_handle != DDS_SECURITY_HANDLE_NIL)
  {
    if (!sc->authentication_context->return_identity_handle(sc->authentication_context, pm->proxypp_identity_handle, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return remote identity handle");
  }
  ddsrt_free(pm);
}

static void pp_proxypp_unrelate(struct dds_security_context *sc, struct participant *pp, const ddsi_guid_t *proxypp_guid)
{
  struct pp_proxypp_match *pm;
  ddsrt_avl_dpath_t dpath;

  ddsrt_mutex_lock(&pp->sec_attr->lock);
  if ((pm = ddsrt_avl_clookup_dpath(&pp_proxypp_treedef, &pp->sec_attr->proxy_participants, proxypp_guid, &dpath)) != NULL)
  {
    ddsrt_avl_cdelete_dpath(&pp_proxypp_treedef, &pp->sec_attr->proxy_participants, pm, &dpath);
    pp_proxypp_match_free(sc, pm);
  }
  ddsrt_mutex_unlock(&pp->sec_attr->lock);
}

static void proxypp_pp_unrelate(struct dds_security_context *sc, struct proxy_participant *proxypp, const ddsi_guid_t *pp_guid, int64_t pp_crypto_handle)
{
  if (proxypp->sec_attr)
  {
    struct proxypp_pp_match *pm;
    struct security_entity_match *match;
    ddsrt_avl_dpath_t dpath;

    match = remove_entity_match(proxypp->e.gv->security_matches, &proxypp->e.guid, pp_guid);
    if (match)
      entity_match_free(match);

    ddsrt_mutex_lock(&proxypp->sec_attr->lock);
    if ((pm = ddsrt_avl_lookup_dpath(&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp_crypto_handle, &dpath)) != NULL)
    {
      ddsrt_avl_delete_dpath(&proxypp_pp_treedef, &proxypp->sec_attr->participants, pm, &dpath);
      proxypp_pp_match_free(sc, pm);
    }
    ddsrt_mutex_unlock(&proxypp->sec_attr->lock);
  }
}

static struct writer_sec_attributes * writer_sec_attributes_new(void)
{
  struct writer_sec_attributes *attr;

  attr = ddsrt_malloc(sizeof(*attr));
  memset(attr, 0, sizeof(*attr));
  attr->crypto_handle = DDS_SECURITY_HANDLE_NIL;
  attr->plugin_attr = false;
  return attr;
}

static void writer_sec_attributes_free(struct writer_sec_attributes *attr)
{
  ddsrt_free(attr);
}

static struct reader_sec_attributes * reader_sec_attributes_new(void)
{
  struct reader_sec_attributes *attr;

  attr = ddsrt_malloc(sizeof(*attr));
  memset(attr, 0, sizeof(*attr));
  attr->crypto_handle = DDS_SECURITY_HANDLE_NIL;
  attr->plugin_attr = false;

  return attr;
}

static void reader_sec_attributes_free(struct reader_sec_attributes *attr)
{
   ddsrt_free(attr);
}

static struct dds_security_garbage *
dds_security_garbage_new(void)
{
  struct dds_security_garbage *sgc;

  sgc = ddsrt_malloc(sizeof(*sgc));
  ddsrt_mutex_init(&sgc->lock);
  ddsrt_avl_init(&participant_garbage_treedef, &sgc->pp_crypto_list);
  return sgc;
}

static void
dds_security_garbage_free(struct dds_security_garbage *sgc)
{
  if (sgc)
  {
    ddsrt_avl_free(&participant_garbage_treedef, &sgc->pp_crypto_list, 0);
    ddsrt_mutex_destroy(&sgc->lock);
    ddsrt_free(sgc);
  }
}

static void
dds_security_garbage_add_pp_crypto(struct dds_security_garbage *sgc, struct cleanup_participant_crypto *info)
{
  ddsrt_mutex_lock(&sgc->lock);
  ddsrt_avl_insert(&participant_garbage_treedef, &sgc->pp_crypto_list, info);
  ddsrt_mutex_unlock(&sgc->lock);
}

#if 0
static struct cleanup_participant_crypto *
dds_security_garbage_find_pp_crypto(struct dds_security_garbage *sgc, int64_t crypto_handle)
{
  struct cleanup_participant_crypto *info;
  struct participant_sec_attributes key = { .crypto_handle=crypto_handle };

  ddsrt_mutex_lock(&sgc->lock);
  info = ddsrt_avl_lookup(&participant_garbage_treedef, &sgc->pp_crypto_list, &key);
  ddsrt_mutex_unlock(&sgc->lock);

  return info;
}
#endif

static struct cleanup_participant_crypto *
dds_security_garbage_remove_pp_crypto(struct dds_security_garbage *sgc, int64_t crypto_handle)
{
  struct cleanup_participant_crypto *info;
  struct participant_sec_attributes key = { .crypto_handle=crypto_handle };
  ddsrt_avl_dpath_t dpath;

  ddsrt_mutex_lock(&sgc->lock);
  info = ddsrt_avl_lookup_dpath(&participant_garbage_treedef, &sgc->pp_crypto_list, &key, &dpath);
  if (info)
    ddsrt_avl_delete_dpath(&participant_garbage_treedef, &sgc->pp_crypto_list, info, &dpath);
  ddsrt_mutex_unlock(&sgc->lock);

  return info;
}

#if 0
static uint32_t
get_proxypp_crypto_list(struct participant_sec_attributes *sec_attr, DDS_Security_ParticipantCryptoHandleSeq *hdls)
{
  uint32_t i;
  struct pp_proxypp_match *pm;
  ddsrt_avl_citer_t it;

  ddsrt_mutex_lock(&sec_attr->lock);
  hdls->_length =  hdls->_maximum = (uint32_t)ddsrt_avl_ccount(&sec_attr->proxy_participants);
  hdls->_buffer = NULL;
  if (hdls->_length == 0)
    return 0;

  hdls->_buffer = ddsrt_malloc(sizeof(int64_t));
  for (pm = ddsrt_avl_citer_first(&pp_proxypp_treedef, &sec_attr->proxy_participants, &it), i = 0; pm; pm = ddsrt_avl_citer_next(&it), i++)
    hdls->_buffer[i] = pm->proxypp_crypto_handle;
  ddsrt_mutex_unlock(&sec_attr->lock);
  return hdls->_length;
}

static int64_t
get_first_proxypp_crypto(struct participant_sec_attributes *sec_attr)
{
  int64_t handle = 0;
  struct pp_proxypp_match *pm;

  ddsrt_mutex_lock(&sec_attr->lock);
  pm = ddsrt_avl_croot(&pp_proxypp_treedef, &sec_attr->proxy_participants);
  if (pm)
    handle = pm->proxypp_crypto_handle;
  ddsrt_mutex_unlock(&sec_attr->lock);

  return handle;
}
#endif

bool q_omg_is_security_loaded (dds_security_context *sc)
{
  return (sc->crypto_context != NULL || sc->authentication_context != NULL || sc->access_control_context != NULL);
}

void q_omg_security_init (struct ddsi_domaingv *gv)
{
  dds_security_context *sc;

  sc = ddsrt_malloc (sizeof (dds_security_context));
  memset (sc, 0, sizeof (dds_security_context));

  sc->auth_plugin.name = AUTH_NAME;
  sc->ac_plugin.name = AC_NAME;
  sc->crypto_plugin.name = CRYPTO_NAME;

  ddsrt_mutex_init (&sc->omg_security_lock);
  sc->logcfg = &gv->logconfig;

  gv->security_context = sc;
  gv->security_matches = security_match_index_new();
  gv->security_garbage = dds_security_garbage_new();

  ddsi_handshake_admin_init(gv);
}

/**
 * Releases all plugins
 */
static void release_plugins (dds_security_context *sc)
{
  if (dds_security_plugin_release (&sc->auth_plugin, sc->authentication_context))
    DDS_CERROR (sc->logcfg, "Error occured releasing %s plugin", sc->auth_plugin.name);

  if (dds_security_plugin_release (&sc->crypto_plugin, sc->crypto_context))
    DDS_CERROR (sc->logcfg, "Error occured releasing %s plugin", sc->crypto_plugin.name);

  if (dds_security_plugin_release (&sc->ac_plugin, sc->access_control_context))
    DDS_CERROR (sc->logcfg, "Error occured releasing %s plugin", sc->ac_plugin.name);

  sc->authentication_context = NULL;
  sc->access_control_context = NULL;
  sc->crypto_context = NULL;
}

void q_omg_security_deinit (struct ddsi_domaingv *gv)
{
  assert (gv != NULL);
  assert (gv->security_context != NULL);

  if (gv->security_context->authentication_context != NULL && gv->security_context->access_control_context != NULL && gv->security_context->crypto_context != NULL){
    release_plugins (gv->security_context);
  }

  ddsi_handshake_admin_deinit(gv);

  security_match_index_free(gv->security_matches);
  gv->security_matches = NULL;

  dds_security_garbage_free(gv->security_garbage);
  gv->security_garbage = NULL;

  ddsrt_mutex_destroy (&gv->security_context->omg_security_lock);

  gv->security_context = NULL;
}

static void dds_qos_to_security_plugin_configuration (const dds_qos_t *qos, dds_security_plugin_suite_config *suite_config)
{
  const struct { const char *name; size_t offset; } tab[] = {
    { DDS_SEC_PROP_AUTH_LIBRARY_PATH, offsetof (dds_security_plugin_suite_config, authentication.library_path) },
    { DDS_SEC_PROP_AUTH_LIBRARY_INIT, offsetof (dds_security_plugin_suite_config, authentication.library_init) },
    { DDS_SEC_PROP_AUTH_LIBRARY_FINALIZE, offsetof (dds_security_plugin_suite_config, authentication.library_finalize) },
    { DDS_SEC_PROP_CRYPTO_LIBRARY_PATH, offsetof (dds_security_plugin_suite_config, cryptography.library_path) },
    { DDS_SEC_PROP_CRYPTO_LIBRARY_INIT, offsetof (dds_security_plugin_suite_config, cryptography.library_init) },
    { DDS_SEC_PROP_CRYPTO_LIBRARY_FINALIZE, offsetof (dds_security_plugin_suite_config, cryptography.library_finalize) },
    { DDS_SEC_PROP_ACCESS_LIBRARY_PATH, offsetof (dds_security_plugin_suite_config, access_control.library_path) },
    { DDS_SEC_PROP_ACCESS_LIBRARY_INIT, offsetof (dds_security_plugin_suite_config, access_control.library_init) },
    { DDS_SEC_PROP_ACCESS_LIBRARY_FINALIZE, offsetof (dds_security_plugin_suite_config, access_control.library_finalize) }
  };

  for (size_t i = 0; i < qos->property.value.n; i++)
    for (size_t j = 0; j < sizeof (tab) / sizeof (tab[0]); j++)
      if (strcmp (qos->property.value.props[i].name, tab[j].name) == 0)
        *((char **) ((char *) suite_config + tab[j].offset)) = ddsrt_strdup (qos->property.value.props[i].value);
}

static void deinit_plugin_config (dds_security_plugin_config *plugin_config)
{
  ddsrt_free (plugin_config->library_path);
  ddsrt_free (plugin_config->library_init);
  ddsrt_free (plugin_config->library_finalize);
}

static void deinit_plugin_suite_config (dds_security_plugin_suite_config *suite_config)
{
  deinit_plugin_config (&suite_config->access_control);
  deinit_plugin_config (&suite_config->authentication);
  deinit_plugin_config (&suite_config->cryptography);
}

dds_return_t q_omg_security_load (dds_security_context *sc, const dds_qos_t *qos)
{
  dds_security_plugin_suite_config psc;
  memset (&psc, 0, sizeof (psc));

  ddsrt_mutex_lock (&sc->omg_security_lock);

  /* Get plugin information */
  dds_qos_to_security_plugin_configuration (qos, &psc);

  /* Check configuration content */
  if (dds_security_check_plugin_configuration (&psc, sc->logcfg) != DDS_RETCODE_OK)
    goto error;

  if (dds_security_load_security_library (&psc.authentication, &sc->auth_plugin, (void **) &sc->authentication_context, sc->logcfg) != DDS_RETCODE_OK)
  {
    DDS_CERROR (sc->logcfg, "Could not load %s plugin.\n", sc->auth_plugin.name);
    goto error;
  }
  if (dds_security_load_security_library (&psc.access_control, &sc->ac_plugin, (void **) &sc->access_control_context, sc->logcfg) != DDS_RETCODE_OK)
  {
    DDS_CERROR (sc->logcfg, "Could not load %s library\n", sc->ac_plugin.name);
    goto error;
  }
  if (dds_security_load_security_library (&psc.cryptography, &sc->crypto_plugin, (void **) &sc->crypto_context, sc->logcfg) != DDS_RETCODE_OK)
  {
    DDS_CERROR (sc->logcfg, "Could not load %s library\n", sc->crypto_plugin.name);
    goto error;
  }

  /* now check if all plugin functions are implemented */
  if (dds_security_verify_plugin_functions (
              sc->authentication_context, &sc->auth_plugin,
              sc->crypto_context, &sc->crypto_plugin,
              sc->access_control_context, &sc->ac_plugin,
              sc->logcfg) != DDS_RETCODE_OK)
  {
    goto error_verify;
  }

  /* Add listeners */
#if LISTENERS_IMPLEMENTED
  if (!access_control_context->set_listener (access_control_context, &listener_ac, &ex))
  {
    DDS_CERROR (sc->logcfg, "Could not set access_control listener: %s\n", ex.message ? ex.message : "<unknown error>");
    goto error_set_ac_listener;
  }
  if (!authentication_context->set_listener (authentication_context, &listener_auth, &ex))
  {
    DDS_CERROR (sc->logcfg, "Could not set authentication listener: %s\n", ex.message ? ex.message : "<unknown error>");
    goto err_set_auth_listener;
  }
#endif

#if HANDSHAKE_IMPLEMENTED
    (void) q_handshake_initialize ();
#endif

  deinit_plugin_suite_config (&psc);
  ddsrt_mutex_unlock (&sc->omg_security_lock);
  DDS_CLOG (DDS_LC_TRACE, sc->logcfg, "DDS Security plugins have been loaded\n");
  return DDS_RETCODE_OK;

#if LISTENERS_IMPLEMENTED
error_set_auth_listener:
  access_control_context->set_listener (access_control_context, NULL, &ex);
error_set_ac_listener:
#endif
error_verify:
  release_plugins (sc);
error:
  deinit_plugin_suite_config (&psc);
  ddsrt_mutex_unlock (&sc->omg_security_lock);
  return DDS_RETCODE_ERROR;
}

static const char * get_builtin_topic_name(ddsi_entityid_t id)
{
  switch (id.u) {
  case NN_ENTITYID_SEDP_BUILTIN_TOPIC_WRITER:
  case NN_ENTITYID_SEDP_BUILTIN_TOPIC_READER:
    return "DCPSTopic";
    break;
  case NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER:
  case NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_READER:
    return "DCPSPublication";
    break;
  case NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER:
  case NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_READER:
    return "DCPSSubscription";
    break;
  case NN_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER:
  case NN_ENTITYID_SPDP_BUILTIN_PARTICIPANT_READER:
    return "DCPSParticipant";
    break;
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_WRITER:
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_READER:
    return "DCPSParticipantMessage";
    break;
  case NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_WRITER:
  case NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_READER:
    return "DCPSPublicationsSecure";
    break;
  case NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_WRITER:
  case NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_READER:
    return "DCPSSubscriptionsSecure";
    break;
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_STATELESS_MESSAGE_WRITER:
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_STATELESS_MESSAGE_READER:
    return "DCPSParticipantStatelessMessage";
    break;
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_WRITER:
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_READER:
    return "DCPSParticipantMessageSecure";
    break;
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER:
  case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_READER:
    return "DCPSParticipantVolatileMessageSecure";
    break;
  case NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER:
  case NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_READER:
    return "DCPSParticipantsSecure";
    break;
  default:
    return "(null)";
    break;
  }

  return NULL;
}

static void notify_handshake_recv_token(struct participant *pp, struct proxy_participant *proxypp)
{
  struct ddsi_handshake *handshake;

  handshake = ddsi_handshake_find(pp, proxypp);
  if (handshake) {
    ddsi_handshake_crypto_tokens_received(handshake);
    ddsi_handshake_release(handshake);
  }
}

static const char * get_reader_topic_name(struct reader *rd)
{
  if (rd->topic) {
    return rd->topic->name;
  }
  return get_builtin_topic_name(rd->e.guid.entityid);
}

static const char * get_writer_topic_name(struct writer *wr)
{
  if (wr->topic) {
    return wr->topic->name;
  }
  return get_builtin_topic_name(wr->e.guid.entityid);
}

bool q_omg_participant_is_secure(const struct participant *pp)
{
  return ((pp->sec_attr != NULL) && (pp->sec_attr->crypto_handle != DDS_SECURITY_HANDLE_NIL));
}

bool q_omg_proxy_participant_is_secure (const struct proxy_participant *proxypp)
{
  return (proxypp->sec_attr != NULL);
}

bool q_omg_participant_allow_unauthenticated(struct participant *pp)
{
  return ((pp->sec_attr != NULL) && pp->sec_attr->attr.allow_unauthenticated_participants);
}

bool q_omg_security_check_create_participant(struct participant *pp, uint32_t domain_id)
{
  bool allowed = false;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_IdentityHandle identity_handle = DDS_SECURITY_HANDLE_NIL;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_ValidationResult_t result = 0;
  DDS_Security_IdentityToken identity_token;
  DDS_Security_PermissionsToken permissions_token = DDS_SECURITY_TOKEN_INIT;
  DDS_Security_PermissionsCredentialToken credential_token = DDS_SECURITY_TOKEN_INIT;
  DDS_Security_Qos par_qos;
  ddsi_guid_t candidate_guid;
  ddsi_guid_t adjusted_guid;

  if (!sc)
    return true;

  /* Validate local identity */
  ETRACE (pp, "validate_local_identity: candidate_guid: "PGUIDFMT" ", PGUID (pp->e.guid));

  pp->sec_attr = ddsrt_malloc(sizeof(struct participant_sec_attributes));
  memset(pp->sec_attr, 0, sizeof(struct participant_sec_attributes));

  candidate_guid = nn_hton_guid(pp->e.guid);
  q_omg_shallow_copy_security_qos(&par_qos, &(pp->plist->qos));

  result = sc->authentication_context->validate_local_identity(
      sc->authentication_context, &identity_handle,
      (DDS_Security_GUID_t *) &adjusted_guid, (DDS_Security_DomainId) domain_id, &par_qos,
      (DDS_Security_GUID_t *) &candidate_guid, &exception);
  if (result != DDS_SECURITY_VALIDATION_OK)
  {
    EXCEPTION_ERROR(sc, &exception, "Error occurred while validating local permission");
    goto validation_failed;
  }
  pp->e.guid = nn_ntoh_guid(adjusted_guid);
  pp->local_identity_handle = identity_handle;

  ETRACE (pp, "adjusted_guid: "PGUIDFMT" ", PGUID (pp->e.guid));

  DDS_Security_Exception_reset(&exception);

  /* Get the identity token and add this to the plist of the participant */
  if (!sc->authentication_context->get_identity_token(sc->authentication_context, &identity_token, identity_handle, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Error occurred while retrieving the identity token");
    goto validation_failed;
  }
  assert(exception.code == 0);

  q_omg_security_dataholder_copyin(&pp->plist->identity_token, &identity_token);
  DDS_Security_DataHolder_deinit(&identity_token);
  pp->plist->present |= PP_IDENTITY_TOKEN;

  pp->permissions_handle = sc->access_control_context->validate_local_permissions(
       sc->access_control_context, sc->authentication_context, identity_handle,
       (DDS_Security_DomainId)domain_id, &par_qos, &exception);
  if (pp->permissions_handle == DDS_SECURITY_HANDLE_NIL)
  {
    EXCEPTION_ERROR(sc, &exception, "Error occured while validating local permissions");
    goto not_allowed;
  }

  /* ask to access control security plugin for create participant permissions related to this identity*/
  allowed = sc->access_control_context->check_create_participant(sc->access_control_context, pp->permissions_handle, (DDS_Security_DomainId) domain_id, &par_qos, &exception);
  if (!allowed)
  {
    EXCEPTION_ERROR(sc, &exception, "It is not allowed to create participant");
    goto not_allowed;
  }

  /* Get the identity token and add this to the plist of the participant */
  if (!sc->access_control_context->get_permissions_token(sc->access_control_context, &permissions_token, pp->permissions_handle, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Error occurred while retrieving the permissions token");
    goto not_allowed;
  }

  q_omg_security_dataholder_copyin(&pp->plist->permissions_token, &permissions_token);
  pp->plist->present |= PP_PERMISSIONS_TOKEN;

  if (!sc->access_control_context->get_permissions_credential_token(sc->access_control_context, &credential_token, pp->permissions_handle, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Error occurred while retrieving the permissions credential token");
    goto no_credentials;
  }

  if (!sc->authentication_context->set_permissions_credential_and_token(sc->authentication_context, pp->local_identity_handle, &credential_token, &permissions_token, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Error occurred while setting the permissions credential token");
    goto no_credentials;
  }

  if (!sc->access_control_context->get_participant_sec_attributes(sc->access_control_context, pp->permissions_handle, &pp->sec_attr->attr, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to get participant security attributes");
    goto no_sec_attr;
  }

  pp->sec_attr->plugin_attr = true;
  pp->sec_attr->crypto_handle = sc->crypto_context->crypto_key_factory->register_local_participant(
            sc->crypto_context->crypto_key_factory, pp->local_identity_handle, pp->permissions_handle, NULL, &pp->sec_attr->attr, &exception);
  if (!pp->sec_attr->crypto_handle) {
    EXCEPTION_ERROR(sc, &exception, "Failed to register participant with crypto key factory");
    goto no_crypto;
  }

  ETRACE (pp, "\n");

  allowed = true;

no_crypto:
no_sec_attr:
  if (permissions_token.class_id)
    (void)sc->access_control_context->return_permissions_token(sc->access_control_context, &permissions_token, NULL);
  if (credential_token.class_id)
    (void)sc->access_control_context->return_permissions_credential_token(sc->access_control_context, &credential_token, NULL);
no_credentials:
  (void)sc->access_control_context->return_permissions_token(sc->access_control_context, &permissions_token, NULL);
not_allowed:
validation_failed:
  q_omg_shallow_free_security_qos(&par_qos);
  return allowed;
}

static void cleanup_participant_crypto_handle(void *arg)
{
  struct cleanup_participant_crypto *info = arg;
  struct cleanup_participant_crypto *n;
  struct dds_security_context *sc = info->sc;
  struct ddsi_domaingv *gv = info->gv;
  struct pp_proxypp_match *pm;

  n = dds_security_garbage_remove_pp_crypto(gv->security_garbage, info->sec_attr->crypto_handle);
  assert(n == info);
  (void)n;

  pm = ddsrt_avl_cfind_min(&pp_proxypp_treedef, &info->sec_attr->proxy_participants);
  while (pm)
  {
    struct pp_proxypp_match *next = ddsrt_avl_cfind_succ(&pp_proxypp_treedef, &info->sec_attr->proxy_participants, pm);
    struct proxy_participant *proxypp = entidx_lookup_proxy_participant_guid(gv->entity_index, &pm->proxypp_guid);
    if (proxypp)
      proxypp_pp_unrelate(sc, proxypp, &info->guid, info->sec_attr->crypto_handle);
    ddsrt_avl_cdelete(&pp_proxypp_treedef, &info->sec_attr->proxy_participants, pm);
    ddsrt_free(pm);
    pm = next;
  }

  (void)info->sc->crypto_context->crypto_key_factory->unregister_participant(info->sc->crypto_context->crypto_key_factory, info->sec_attr->crypto_handle, NULL);

  ddsrt_avl_cfree(&pp_proxypp_treedef, &info->sec_attr->proxy_participants, NULL);
  ddsrt_mutex_unlock(&info->sec_attr->lock);
  ddsrt_free(info->sec_attr);
  ddsrt_free(arg);
}

void q_omg_security_deregister_participant(struct participant *pp)
{
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);

  if (!sc)
    return;

  /* When the participant is deleted the timed event queue may still contain
   * messages from this participant. Therefore the crypto handle should still
   * be available to ensure that the rtps message can be encoded.
   * For this purpose the cleanup of the associated crypto handle is delayed.
   * A callback is scheduled to be called after some delay to cleanup this
   * crypto handle.
   */
  if (pp->sec_attr->crypto_handle != DDS_SECURITY_HANDLE_NIL) {
    struct cleanup_participant_crypto *arg = ddsrt_malloc (sizeof (*arg));
    arg->gv = pp->e.gv;
    arg->sc = sc;
    arg->guid = pp->e.guid;
    arg->sec_attr = pp->sec_attr;
    dds_security_garbage_add_pp_crypto(arg->gv->security_garbage, arg);
    qxev_nt_callback(pp->e.gv->xevents, cleanup_participant_crypto_handle, arg);
  }

  if (pp->permissions_handle != DDS_SECURITY_HANDLE_NIL)
  {
    if (!sc->access_control_context->return_permissions_handle(sc->access_control_context, pp->permissions_handle, &exception))
    {
      EXCEPTION_ERROR(sc, &exception, "Failed to return permissions handle");
    }
  }
  if (pp->local_identity_handle != DDS_SECURITY_HANDLE_NIL)
  {
    if (!sc->authentication_context->return_identity_handle(sc->authentication_context, pp->local_identity_handle, &exception))
    {
      EXCEPTION_ERROR(sc, &exception, "Failed to return identity handle");
    }

  }
  if (pp->sec_attr->plugin_attr)
  {
    if (!sc->access_control_context->return_participant_sec_attributes(sc->access_control_context, &pp->sec_attr->attr, &exception))
    {
      EXCEPTION_ERROR(sc, &exception, "Failed to return participant security attributes");
    }
  }
  pp->sec_attr = NULL;
}

int64_t q_omg_security_get_local_participant_handle(const struct participant *pp)
{
  if (pp->sec_attr)
    return pp->sec_attr->crypto_handle;
  return 0;
}

bool q_omg_participant_is_access_protected(const struct participant *pp)
{
  return ((pp->sec_attr != NULL) && pp->sec_attr->attr.is_access_protected);
}

bool q_omg_participant_is_rtps_protected(const struct participant *pp)
{
  return ((pp->sec_attr != NULL) && pp->sec_attr->attr.is_rtps_protected);
}

bool q_omg_participant_is_liveliness_protected(const struct participant *pp)
{
  return ((pp->sec_attr != NULL) && pp->sec_attr->attr.is_liveliness_protected);
}


static bool maybe_rtps_protected(ddsi_entityid_t entityid)
{
  if (!is_builtin_entityid(entityid, NN_VENDORID_ECLIPSE))
    return true;

  switch (entityid.u)
  {
    case NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_WRITER:
    case NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_READER:
    case NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_WRITER:
    case NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_READER:
    case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_WRITER:
    case NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_READER:
    case NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER:
    case NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_READER:
      return true;
    default:
      return false;
  }
}

bool q_omg_security_is_remote_rtps_protected(const struct proxy_participant *proxypp, ddsi_entityid_t entityid)
{
  return q_omg_proxy_participant_is_secure(proxypp) &&
    SECURITY_INFO_IS_RTPS_PROTECTED(proxypp->security_info) &&
    maybe_rtps_protected(entityid);
}

bool q_omg_security_is_local_rtps_protected(const struct participant *pp, ddsi_entityid_t entityid)
{
  return q_omg_participant_is_rtps_protected(pp) && maybe_rtps_protected(entityid);
}

bool q_omg_get_participant_security_info(const struct participant *pp, nn_security_info_t *info)
{
  assert(pp);
  assert(info);

  if (q_omg_participant_is_secure(pp)) {
    const DDS_Security_ParticipantSecurityAttributes *attr = &(pp->sec_attr->attr);

    info->security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    info->plugin_security_attributes = attr->plugin_participant_attributes;

    if (attr->is_discovery_protected)
      info->security_attributes |= NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_DISCOVERY_PROTECTED;

    if (attr->is_liveliness_protected)
      info->security_attributes |= NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_LIVELINESS_PROTECTED;

    if (attr->is_rtps_protected)
      info->security_attributes |= NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_RTPS_PROTECTED;

    return true;
  }

  info->security_attributes = 0;
  info->plugin_security_attributes = 0;

  return false;
}

static void q_omg_get_endpoint_security_info(DDS_Security_EndpointSecurityAttributes *attr, nn_security_info_t *info)
{
    info->security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    info->plugin_security_attributes = attr->plugin_endpoint_attributes;

    if (attr->is_read_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_READ_PROTECTED;

    if (attr->is_write_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_WRITE_PROTECTED;

    if (attr->is_discovery_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_DISCOVERY_PROTECTED;

    if (attr->is_liveliness_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_LIVELINESS_PROTECTED;

    if (attr->is_submessage_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED;

    if (attr->is_payload_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_PAYLOAD_PROTECTED;

    if (attr->is_key_protected)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_KEY_PROTECTED;
}

static bool is_topic_discovery_protected(DDS_Security_PermissionsHandle permission_handle, dds_security_access_control *access_control, const char *topic_name)
{
  DDS_Security_TopicSecurityAttributes attributes = {0,0,0,0};
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;

  if (access_control->get_topic_sec_attributes(access_control, permission_handle, topic_name, &attributes, &exception))
    return attributes.is_discovery_protected;
  else
    DDS_Security_Exception_reset(&exception);
  return false;
}

bool q_omg_security_check_create_topic(const struct ddsi_domaingv *gv, const ddsi_guid_t *pp_guid, const char *topic_name, const struct dds_qos *qos)
{
  bool result = true;
  struct participant *pp;
  struct dds_security_context *sc;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_Qos topic_qos;

  thread_state_awake (lookup_thread_state (), gv);
  pp = entidx_lookup_participant_guid (gv->entity_index, pp_guid);

  if ((sc = q_omg_security_get_secure_context(pp)) != NULL)
  {
    q_omg_shallow_copy_security_qos(&topic_qos, qos);
    result = sc->access_control_context->check_create_topic(sc->access_control_context, pp->permissions_handle, (DDS_Security_DomainId)gv->config.domainId, topic_name, &topic_qos, &exception);
    if (!result)
    {
      /*log if the topic discovery is not protected*/
      if (!is_topic_discovery_protected(pp->permissions_handle, sc->access_control_context, topic_name))
        EXCEPTION_ERROR(sc, &exception, "Local topic permission denied");
      else
        DDS_Security_Exception_reset(&exception);
    }
    q_omg_shallow_free_security_qos(&topic_qos);
  }
  thread_state_asleep (lookup_thread_state ());

  return result;
}

bool q_omg_security_check_create_writer(struct participant *pp, uint32_t domain_id, const char *topic_name, const struct dds_qos *writer_qos)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp) ;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_PartitionQosPolicy partitions;
  DDS_Security_Qos security_qos;
  bool result;

  if (!sc)
    return true;

  if (writer_qos->present & QP_PARTITION)
    g_omg_shallow_copy_StringSeq(&partitions.name, &(writer_qos->partition));
  else
    memset(&(partitions), 0, sizeof(DDS_Security_PartitionQosPolicy));

  q_omg_shallow_copy_security_qos(&security_qos, writer_qos);

  result = sc->access_control_context->check_create_datawriter(sc->access_control_context, pp->permissions_handle, (DDS_Security_DomainId)domain_id, topic_name, &security_qos, &partitions, NULL, &exception);
  if (!result)
  {
    /*log if the topic discovery is not protected*/
    if (!is_topic_discovery_protected( pp->permissions_handle, sc->access_control_context, topic_name))
      EXCEPTION_ERROR(sc, &exception, "Local topic permission denied");
    else
      DDS_Security_Exception_reset(&exception);
  }

  q_omg_shallow_free_security_qos(&security_qos);
  g_omg_shallow_free_StringSeq(&partitions.name);

  return result;
}

void q_omg_security_register_writer(struct writer *wr)
{
  struct participant *pp = wr->c.pp;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_PartitionQosPolicy partitions;
  DDS_Security_PropertySeq properties;
  const char *topic_name;

  if (!sc)
    return;

  if (wr->xqos->present & QP_PARTITION)
    g_omg_shallow_copy_StringSeq(&partitions.name, &(wr->xqos->partition));
  else
    memset(&(partitions), 0, sizeof(DDS_Security_PartitionQosPolicy));

  wr->sec_attr = writer_sec_attributes_new();
  topic_name = get_writer_topic_name(wr);
  if (!sc->access_control_context->get_datawriter_sec_attributes(sc->access_control_context, pp->permissions_handle, topic_name, &partitions, NULL, &wr->sec_attr->attr, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to retrieve writer security attributes");
    goto no_attr;
  }
  wr->sec_attr->plugin_attr = true;

  if (wr->sec_attr->attr.is_payload_protected || wr->sec_attr->attr.is_submessage_protected)
  {
    if (wr->xqos->present & QP_PROPERTY_LIST)
      q_omg_copy_PropertySeq(&properties, &wr->xqos->property.value);
    else
      memset(&properties, 0, sizeof(DDS_Security_PropertySeq));

    wr->sec_attr->crypto_handle = sc->crypto_context->crypto_key_factory->register_local_datawriter(
        sc->crypto_context->crypto_key_factory, pp->sec_attr->crypto_handle, &properties, &wr->sec_attr->attr, &exception);
    DDS_Security_PropertySeq_freebuf(&properties);
    if (wr->sec_attr->crypto_handle == DDS_SECURITY_HANDLE_NIL)
    {
      EXCEPTION_ERROR(sc, &exception, "Failed to register writer with crypto");
      goto not_registered;
    }
  }

  if (wr->sec_attr->attr.is_key_protected)
    wr->include_keyhash = 1;

not_registered:
no_attr:
  g_omg_shallow_free_StringSeq(&partitions.name);
}

void q_omg_security_deregister_writer(struct writer *wr)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(wr->c.pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;

  if (wr->sec_attr)
  {
    if (wr->sec_attr->crypto_handle != DDS_SECURITY_HANDLE_NIL)
    {
      if (!sc->crypto_context->crypto_key_factory->unregister_datawriter(sc->crypto_context->crypto_key_factory, wr->sec_attr->crypto_handle, &exception))
        EXCEPTION_ERROR(sc, &exception, "Failed to unregister writer with crypto");
    }
    if (wr->sec_attr->plugin_attr)
    {
      if (!sc->access_control_context->return_datawriter_sec_attributes(sc->access_control_context, &wr->sec_attr->attr, &exception))
        EXCEPTION_ERROR(sc, &exception, "Failed to return writer security attributes");
    }
    writer_sec_attributes_free(wr->sec_attr);
    wr->sec_attr = NULL;
  }
}

bool q_omg_get_writer_security_info(const struct writer *wr, nn_security_info_t *info)
{
  assert(wr);
  assert(info);

  if (wr->sec_attr) {
      q_omg_get_endpoint_security_info(&wr->sec_attr->attr, info);
      return true;
  }
  info->plugin_security_attributes = 0;
  info->security_attributes = 0;
  return false;
}

bool q_omg_security_check_create_reader(struct participant *pp, uint32_t domain_id, const char *topic_name, const struct dds_qos *reader_qos)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_PartitionQosPolicy partitions;
  DDS_Security_Qos security_qos;
  bool result;

  if (!sc)
    return true;

  if (reader_qos->present & QP_PARTITION)
    g_omg_shallow_copy_StringSeq(&partitions.name, &(reader_qos->partition));
  else
    memset(&(partitions), 0, sizeof(DDS_Security_PartitionQosPolicy));

  q_omg_shallow_copy_security_qos(&security_qos, reader_qos);

  result = sc->access_control_context->check_create_datareader(sc->access_control_context, pp->permissions_handle, (DDS_Security_DomainId)domain_id, topic_name, &security_qos, &partitions, NULL, &exception);
  if (!result)
  {
    /*log if the topic discovery is not protected*/
    if (!is_topic_discovery_protected( pp->permissions_handle, sc->access_control_context, topic_name))
      EXCEPTION_ERROR(sc, &exception, "Reader is not permitted");
    else
      DDS_Security_Exception_reset(&exception);
  }

  q_omg_shallow_free_security_qos(&security_qos);
  g_omg_shallow_free_StringSeq(&partitions.name);

  return result;
}

void q_omg_security_register_reader(struct reader *rd)
{
  struct participant *pp = rd->c.pp;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_PartitionQosPolicy partitions;
  DDS_Security_PropertySeq properties;
  const char *topic_name;

  if (!sc)
    return;

  if (rd->xqos->present & QP_PARTITION)
    g_omg_shallow_copy_StringSeq(&partitions.name, &(rd->xqos->partition));
  else
    memset(&(partitions), 0, sizeof(DDS_Security_PartitionQosPolicy));

  rd->sec_attr = reader_sec_attributes_new();

  topic_name = get_reader_topic_name(rd);
  if (!sc->access_control_context->get_datareader_sec_attributes(sc->access_control_context, pp->permissions_handle, topic_name, &partitions, NULL, &rd->sec_attr->attr, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to retrieve reader security attributes");
    goto no_attr;
  }
  rd->sec_attr->plugin_attr = true;

  if (rd->sec_attr->attr.is_payload_protected || rd->sec_attr->attr.is_submessage_protected)
  {
    if (rd->xqos->present & QP_PROPERTY_LIST)
      q_omg_copy_PropertySeq(&properties, &rd->xqos->property.value);
    else
      memset(&properties, 0, sizeof(DDS_Security_PropertySeq));

    rd->sec_attr->crypto_handle = sc->crypto_context->crypto_key_factory->register_local_datareader(
        sc->crypto_context->crypto_key_factory, pp->sec_attr->crypto_handle, &properties, &rd->sec_attr->attr, &exception);
    DDS_Security_PropertySeq_freebuf(&properties);
    if (rd->sec_attr->crypto_handle == DDS_SECURITY_HANDLE_NIL)
    {
      EXCEPTION_ERROR(sc, &exception, "Failed to register reader with crypto");
      goto not_registered;
    }
  }

not_registered:
no_attr:
  g_omg_shallow_free_StringSeq(&partitions.name);
}

void q_omg_security_deregister_reader(struct reader *rd)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(rd->c.pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;

  if (rd->sec_attr)
  {
    assert(sc);
    if (rd->sec_attr->crypto_handle != DDS_SECURITY_HANDLE_NIL)
    {
      if (!sc->crypto_context->crypto_key_factory->unregister_datareader(sc->crypto_context->crypto_key_factory, rd->sec_attr->crypto_handle, &exception))
      {
        EXCEPTION_ERROR(sc, &exception, "Failed to unregister reader with crypto");
      }
    }
    if (rd->sec_attr->plugin_attr)
    {
      if (!sc->access_control_context->return_datareader_sec_attributes(sc->access_control_context, &rd->sec_attr->attr, &exception))
      {
        EXCEPTION_ERROR(sc, &exception, "Failed to return reader security attributes");
      }
    }
    reader_sec_attributes_free(rd->sec_attr);
    rd->sec_attr = NULL;
  }
}

bool q_omg_get_reader_security_info(const struct reader *rd, nn_security_info_t *info)
{
  assert(rd);
  assert(info);

  if (rd->sec_attr) {
    q_omg_get_endpoint_security_info(&rd->sec_attr->attr, info);
    return true;
  }
  info->plugin_security_attributes = 0;
  info->security_attributes = 0;
  return false;
}

unsigned determine_subscription_writer(const struct reader *rd)
{
  if (q_omg_reader_is_discovery_protected (rd))
    return NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_WRITER;
  else
    return NN_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER;
}

unsigned determine_publication_writer (const struct writer *wr)
{
  if (q_omg_writer_is_discovery_protected (wr))
    return NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_WRITER;
  else
    return NN_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER;
}

static int64_t check_remote_participant_permissions(uint32_t domain_id, struct participant *pp, struct proxy_participant *proxypp, int64_t remote_identity_handle)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct ddsi_handshake *handshake;
  DDS_Security_PermissionsToken permissions_token = DDS_SECURITY_TOKEN_INIT;
  DDS_Security_AuthenticatedPeerCredentialToken peer_credential_token = DDS_SECURITY_TOKEN_INIT;
  int64_t permissions_hdl = DDS_SECURITY_HANDLE_NIL;

  if (proxypp->plist->present & PP_PERMISSIONS_TOKEN)
      q_omg_shallow_copyin_DataHolder(&permissions_token, &proxypp->plist->permissions_token);
  else
      memset(&permissions_token, 0, sizeof(DDS_Security_PermissionsToken));

  handshake = ddsi_handshake_find(pp, proxypp);
  if (!handshake)
  {
    ELOG(DDS_LC_ERROR, pp, "Could not find handshake local participant "PGUIDFMT" and remote participant "PGUIDFMT,
                PGUID(pp->e.guid), PGUID(proxypp->e.guid));
      goto no_handshake;
  }

  if (!sc->authentication_context->get_authenticated_peer_credential_token(sc->authentication_context, &peer_credential_token, ddsi_handshake_get_handle(handshake), &exception))
  {
    if (q_omg_participant_is_access_protected(pp))
    {
      EXCEPTION_ERROR(sc, &exception, "Could not authenticate_peer_credential_token for local participan1152t "PGUIDFMT" and remote participant "PGUIDFMT,
          PGUID(pp->e.guid), PGUID(proxypp->e.guid));
      goto no_credentials;
    }
    /* Failing is allowed due to the non-protection of access. */
    EXCEPTION_WARNING(sc, &exception, "Could not authenticate_peer_credential_token for local participant "PGUIDFMT" and remote participant "PGUIDFMT ,
        PGUID(pp->e.guid), PGUID(proxypp->e.guid));
  }

  permissions_hdl = sc->access_control_context->validate_remote_permissions(
      sc->access_control_context, sc->authentication_context, pp->local_identity_handle, remote_identity_handle, &permissions_token, &peer_credential_token, &exception);
  if (permissions_hdl == DDS_SECURITY_HANDLE_NIL)
  {
    if (q_omg_participant_is_access_protected(pp))
    {
      EXCEPTION_ERROR(sc, &exception, "Could not get remote participant "PGUIDFMT" permissions from plugin", PGUID(proxypp->e.guid));
      goto no_permissions;
    }
    /* Failing is allowed due to the non-protection of access. */
    EXCEPTION_WARNING(sc, &exception, "Could not get remote participant "PGUIDFMT" permissions from plugin", PGUID(proxypp->e.guid));
  }

  /* Only check remote participant if joining access is protected. */
  if (q_omg_participant_is_access_protected(pp))
  {
    DDS_Security_ParticipantBuiltinTopicDataSecure participant_data;

    q_omg_shallow_copy_ParticipantBuiltinTopicDataSecure(&participant_data, &(proxypp->e.guid), proxypp->plist);
    if (!sc->access_control_context->check_remote_participant(sc->access_control_context, permissions_hdl, (DDS_Security_DomainId)domain_id, &participant_data, &exception))
    {
      EXCEPTION_WARNING(sc, &exception, "Plugin does not allow remote participant "PGUIDFMT,  PGUID(proxypp->e.guid));
      if (!sc->access_control_context->return_permissions_handle(sc->access_control_context, permissions_hdl, &exception))
      {
        EXCEPTION_ERROR(sc, &exception, "Failed to return permissions handle");
      }
      permissions_hdl = DDS_SECURITY_HANDLE_NIL;
    }
    q_omg_shallow_free_ParticipantBuiltinTopicDataSecure(&participant_data);
  }

no_permissions:
  if (!sc->authentication_context->return_authenticated_peer_credential_token(sc->authentication_context, &peer_credential_token, &exception))
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to return peer credential token");
  }
no_credentials:
  ddsi_handshake_release(handshake);
no_handshake:
  q_omg_shallow_free_DataHolder(&permissions_token);
  return permissions_hdl;
}

static void send_participant_crypto_tokens(struct participant *pp, struct proxy_participant *proxypp, DDS_Security_ParticipantCryptoHandle local_crypto, DDS_Security_ParticipantCryptoHandle remote_crypto)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_ParticipantCryptoTokenSeq tokens = DDS_SECURITY_SEQUENCE_INIT;
  bool r;

  r = sc->crypto_context->crypto_key_exchange->create_local_participant_crypto_tokens(sc->crypto_context->crypto_key_exchange, &tokens, local_crypto, remote_crypto, &exception);
  if (!r)
    EXCEPTION_ERROR(sc, &exception, "Failed to create local participant crypto tokens "PGUIDFMT" for remote participant "PGUIDFMT,  PGUID(pp->e.guid), PGUID(proxypp->e.guid));
  else
  {
    nn_dataholderseq_t tholder;

    q_omg_shallow_copyout_DataHolderSeq(&tholder, &tokens);
    write_crypto_participant_tokens(pp, proxypp, &tholder);
    q_omg_shallow_free_nn_dataholderseq(&tholder);

    if (!sc->crypto_context->crypto_key_exchange->return_crypto_tokens(sc->crypto_context->crypto_key_exchange, &tokens, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return local participant crypto tokens "PGUIDFMT" for remote participant "PGUIDFMT, PGUID(pp->e.guid), PGUID(proxypp->e.guid));
  }
}

static int64_t get_permissions_handle(struct participant *pp, struct proxy_participant *proxypp)
{
  int64_t hdl = 0;
  struct proxypp_pp_match *pm;

  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  pm = ddsrt_avl_lookup(&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp->sec_attr->crypto_handle);
  if (pm)
    hdl = pm->permissions_handle;
  ddsrt_mutex_unlock(&proxypp->sec_attr->lock);

  return hdl;
}

void q_omg_security_init_remote_participant(struct proxy_participant *proxypp)
{
  proxypp->sec_attr = ddsrt_malloc(sizeof(*proxypp->sec_attr));
  ddsrt_mutex_init(&proxypp->sec_attr->lock);
  ddsrt_avl_init (&proxypp_pp_treedef, &proxypp->sec_attr->participants);
  proxypp->sec_attr->sc = proxypp->e.gv->security_context;
}

static bool proxypp_is_authenticated(const struct proxy_participant *proxypp)
{
  bool authenticated = false;

  if (proxypp->sec_attr)
  {
    ddsrt_mutex_lock(&proxypp->sec_attr->lock);
    authenticated = !ddsrt_avl_is_empty(&proxypp->sec_attr->participants);
    ddsrt_mutex_unlock(&proxypp->sec_attr->lock);
  }
  return authenticated;
}

static void match_proxypp_pp(struct participant *pp, struct proxy_participant *proxypp, DDS_Security_IdentityHandle remote_identity_handle, DDS_Security_ParticipantCryptoHandle proxypp_crypto_handle, DDS_Security_PermissionsHandle permissions_handle, DDS_Security_SharedSecretHandle shared_secret_handle)
{
  struct proxypp_pp_match *pm;
  struct pp_proxypp_match *pc;

  pm = proxypp_pp_match_new(pp, remote_identity_handle, proxypp_crypto_handle, permissions_handle, shared_secret_handle);
  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  ddsrt_avl_insert(&proxypp_pp_treedef, &proxypp->sec_attr->participants, pm);
  ddsrt_mutex_unlock(&proxypp->sec_attr->lock);

  pc = pp_proxypp_match_new(proxypp, proxypp_crypto_handle);

  ddsrt_mutex_lock(&pp->sec_attr->lock);
  ddsrt_avl_cinsert(&pp_proxypp_treedef, &pp->sec_attr->proxy_participants, pc);
  ddsrt_mutex_unlock(&pp->sec_attr->lock);
}

bool q_omg_security_register_remote_participant(struct participant *pp, struct proxy_participant *proxypp, int64_t remote_identity_handle, int64_t shared_secret)
{
  bool ret = true;
  struct ddsi_domaingv *gv = pp->e.gv;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_ParticipantCryptoHandle crypto_handle;
  int64_t permissions_handle;
  struct security_entity_match *m;

  permissions_handle = check_remote_participant_permissions(gv->config.domainId, pp, proxypp, remote_identity_handle);
  if (permissions_handle == 0)
    return false;

  GVTRACE("register remote participant "PGUIDFMT" with "PGUIDFMT"\n", PGUID(proxypp->e.guid), PGUID(pp->e.guid));

  crypto_handle = sc->crypto_context->crypto_key_factory->register_matched_remote_participant(
      sc->crypto_context->crypto_key_factory, pp->sec_attr->crypto_handle,
      remote_identity_handle, permissions_handle, shared_secret, &exception);
  if (crypto_handle == DDS_SECURITY_HANDLE_NIL)
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to register matched remote participant "PGUIDFMT" with participant "PGUIDFMT, PGUID(proxypp->e.guid), PGUID(pp->e.guid));
    ret = false;
    goto register_failed;
  }

  m = find_or_create_entity_match(gv->security_matches, &proxypp->e.guid, &pp->e.guid);
  m->crypto_handle = crypto_handle;

  GVTRACE("match pp->crypto=%"PRId64" proxypp->crypto=%"PRId64"\n", pp->sec_attr->crypto_handle, crypto_handle);

  match_proxypp_pp(pp, proxypp, remote_identity_handle, crypto_handle, permissions_handle, shared_secret);

  GVTRACE("create proxypp-pp match pp="PGUIDFMT" proxypp="PGUIDFMT" lidh=%"PRId64, PGUID(pp->e.guid), PGUID(proxypp->e.guid), pp->local_identity_handle);

  if (m->tokens)
  {
    ret = sc->crypto_context->crypto_key_exchange->set_remote_participant_crypto_tokens(sc->crypto_context->crypto_key_exchange, pp->sec_attr->crypto_handle, crypto_handle, m->tokens, &exception);
    if (ret)
    {
      m->matched = true;
      DDS_Security_ParticipantCryptoTokenSeq_free(m->tokens);
      m->tokens = NULL;
      GVTRACE("set participant tokens src("PGUIDFMT") to dst("PGUIDFMT") (by registering remote)\n", PGUID(proxypp->e.guid), PGUID(pp->e.guid));
    }
    else
    {
      EXCEPTION_ERROR(sc, &exception, "Failed to set remote participant crypto tokens "PGUIDFMT" --> "PGUIDFMT, PGUID(proxypp->e.guid), PGUID(pp->e.guid));
      ret = false;
    }
  }

register_failed:
  return ret;
}

void q_omg_security_deregister_remote_participant(struct proxy_participant *proxypp)
{
  struct ddsi_domaingv *gv = proxypp->e.gv;

  if (proxypp->sec_attr) {
    dds_security_context *sc = proxypp->sec_attr->sc;
    struct proxypp_pp_match *pm;
    struct participant *pp;

    pm = ddsrt_avl_find_min(&proxypp_pp_treedef, &proxypp->sec_attr->participants);
    while (pm)
    {
      struct proxypp_pp_match *next = ddsrt_avl_find_succ(&proxypp_pp_treedef, &proxypp->sec_attr->participants, pm);

      ddsrt_avl_delete(&proxypp_pp_treedef, &proxypp->sec_attr->participants, pm);
      pp = entidx_lookup_participant_guid(gv->entity_index, &pm->pp_guid);
      if (pp)
        pp_proxypp_unrelate(sc, pp, &proxypp->e.guid);
      proxypp_pp_match_free(sc, pm);
      pm = next;
    }

    ddsrt_mutex_destroy(&proxypp->sec_attr->lock);
    ddsrt_free(proxypp->sec_attr);
    proxypp->sec_attr = NULL;
  }
}

bool is_proxy_participant_deletion_allowed (struct ddsi_domaingv * const gv, const struct ddsi_guid *guid, const ddsi_entityid_t pwr_entityid)
{
  struct proxy_participant *proxypp;

  assert (gv);
  assert (guid);

  /* TODO: Check if the proxy writer guid prefix matches that of the proxy
   *       participant. Deletion is not allowed when they're not equal. */

  /* Always allow deletion from a secure proxy writer. */
  if (pwr_entityid.u == NN_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER)
    return true;

  /* Not from a secure proxy writer.
   * Only allow deletion when proxy participant is not authenticated. */
  proxypp = entidx_lookup_proxy_participant_guid (gv->entity_index, guid);
  if (!proxypp)
  {
    GVLOGDISC (" unknown");
    return false;
  }

  return (!proxypp_is_authenticated(proxypp));
}

bool q_omg_is_similar_participant_security_info(struct participant *pp, struct proxy_participant *proxypp)
{
  bool matching;
  nn_security_info_t pp_security_info;

  if (!q_omg_get_participant_security_info(pp, &pp_security_info))
    return false;

  matching = SECURITY_INFO_COMPATIBLE(pp_security_info, proxypp->security_info, NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_VALID);
  if (!matching) {
    DDS_CLOG (DDS_LC_WARNING, &pp->e.gv->logconfig, "match remote_participant "PGUIDFMT" with participant "PGUIDFMT" security_attributes mismatch: 0x%08x.0x%08x - 0x%08x.0x%08x\n",
        PGUID(proxypp->e.guid), PGUID(pp->e.guid),
        proxypp->security_info.security_attributes, proxypp->security_info.plugin_security_attributes,
        pp_security_info.security_attributes, pp_security_info.plugin_security_attributes);
  } else {
    /* We previously checked for attribute compatibility. That doesn't
     * mean equal, because compatibility depends on the valid flag.
     * Some products don't properly send the attributes, in which case
     * the valid flag is 0. To be able to support these product, assume
     * that the attributes are the same. If there is actually a mismatch,
     * communication will fail at a later moment anyway. */
    if (!SECURITY_ATTR_IS_VALID(proxypp->security_info.security_attributes)) {
      proxypp->security_info.security_attributes = pp_security_info.security_attributes;
    }
    if (!SECURITY_ATTR_IS_VALID(proxypp->security_info.plugin_security_attributes)) {
      proxypp->security_info.plugin_security_attributes = pp_security_info.plugin_security_attributes;
    }
  }
  assert(matching);
  return matching;
}

void q_omg_security_set_participant_crypto_tokens(struct participant *pp, struct proxy_participant *proxypp, const nn_dataholderseq_t *tokens)
{
  struct ddsi_domaingv *gv = pp->e.gv;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct proxypp_pp_match *pm;
  DDS_Security_DatawriterCryptoTokenSeq *tseq;
  struct security_entity_match *m;

  if (!sc)
    return;

  m = find_or_create_entity_match(gv->security_matches, &proxypp->e.guid, &pp->e.guid);

  tseq = DDS_Security_DataHolderSeq_alloc();
  q_omg_copyin_DataHolderSeq(tseq, tokens);

  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  if ((pm = ddsrt_avl_lookup (&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp->sec_attr->crypto_handle)) == NULL)
  {
    ddsrt_mutex_unlock(&proxypp->sec_attr->lock);
    GVTRACE("remember participant tokens src("PGUIDFMT") dst("PGUIDFMT")\n", PGUID(proxypp->e.guid), PGUID(pp->e.guid));
    m->tokens = tseq;
    notify_handshake_recv_token(pp, proxypp);
  }
  else if (sc->crypto_context->crypto_key_exchange->set_remote_participant_crypto_tokens(sc->crypto_context->crypto_key_exchange, pp->sec_attr->crypto_handle, pm->proxypp_crypto_handle, tseq, &exception))
  {
    m->matched= true;
    ddsrt_mutex_unlock(&proxypp->sec_attr->lock);
    GVTRACE("set participant tokens src("PGUIDFMT") dst("PGUIDFMT")\n", PGUID(proxypp->e.guid), PGUID(pp->e.guid));
    notify_handshake_recv_token(pp, proxypp);
    DDS_Security_DataHolderSeq_free(tseq);
  }
  else
  {
    ddsrt_mutex_unlock(&proxypp->sec_attr->lock);
    EXCEPTION_ERROR(sc, &exception, "Failed to set remote participant crypto tokens "PGUIDFMT" for participant "PGUIDFMT, PGUID(proxypp->e.guid), PGUID(pp->e.guid));
  }
}

void q_omg_security_participant_send_tokens(struct participant *pp, struct proxy_participant *proxypp)
{
  struct proxypp_pp_match *pm;
  int64_t crypto_handle = 0;

  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  pm = ddsrt_avl_lookup(&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp->sec_attr->crypto_handle);
  if (pm)
    crypto_handle = pm->proxypp_crypto_handle;
  ddsrt_mutex_unlock(&proxypp->sec_attr->lock);

  if (crypto_handle != 0)
    send_participant_crypto_tokens(pp, proxypp, pp->sec_attr->crypto_handle, crypto_handle);
}

int64_t q_omg_security_get_remote_participant_handle(int64_t pp_crypto_handle, struct proxy_participant *proxypp)
{
  struct proxypp_pp_match *pm;
  int64_t handle = 0;

  DDS_CTRACE(&proxypp->e.gv->logconfig, "get_remote_handle proxypp="PGUIDFMT" lidh=%"PRId64, PGUID(proxypp->e.guid), pp_crypto_handle);
  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  if ((pm = ddsrt_avl_lookup (&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp_crypto_handle)) != NULL)
    handle = pm->proxypp_crypto_handle;
  ddsrt_mutex_unlock(&proxypp->sec_attr->lock);

  return handle;
}

void set_proxy_participant_security_info(struct proxy_participant *proxypp, const ddsi_plist_t *plist)
{
  assert(proxypp);
  assert(plist);
  if (plist->present & PP_PARTICIPANT_SECURITY_INFO) {
    proxypp->security_info.security_attributes = plist->participant_security_info.security_attributes;
    proxypp->security_info.plugin_security_attributes = plist->participant_security_info.plugin_security_attributes;
  } else {
    proxypp->security_info.security_attributes = 0;
    proxypp->security_info.plugin_security_attributes = 0;
  }
}

bool q_omg_writer_is_discovery_protected(const struct writer *wr)
{
  assert (wr != NULL);
  return wr->sec_attr != NULL && wr->sec_attr->attr.is_discovery_protected;
}

bool q_omg_writer_is_submessage_protected(const struct writer *wr)
{
  assert (wr != NULL);
  return wr->sec_attr != NULL && wr->sec_attr->attr.is_submessage_protected;
}

bool q_omg_writer_is_payload_protected(const struct writer *wr)
{
  assert (wr != NULL);
  return wr->sec_attr != NULL && wr->sec_attr->attr.is_payload_protected;
}

bool q_omg_security_check_remote_writer_permissions(const struct proxy_writer *pwr, uint32_t domain_id, struct participant *pp)
{
  struct ddsi_domaingv *gv = pp->e.gv;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_PublicationBuiltinTopicDataSecure publication_data;
  bool ok = true;

  if (!sc)
    return true;

  if (!q_omg_proxy_participant_is_secure(pwr->c.proxypp))
  {
    if (q_omg_participant_allow_unauthenticated(pp))
    {
      GVTRACE (" allow non-secure remote writer "PGUIDFMT, PGUID (pwr->e.guid));
      return true;
    }
    else
    {
      GVWARNING("Non secure remote writer "PGUIDFMT" is not allowed.", PGUID(pwr->e.guid));
      return false;
    }
  }

  if (SECURITY_INFO_IS_WRITE_PROTECTED(pwr->security_info))
  {
    DDS_Security_PermissionsHandle permissions_handle;

    if ((permissions_handle = get_permissions_handle(pp, pwr->c.proxypp)) != 0)
    {
      GVTRACE("Secure remote writer "PGUIDFMT" proxypp does not have permissions handle yet\n", PGUID(pwr->e.guid));
      return false;
    }
    else
    {
      q_omg_shallow_copy_PublicationBuiltinTopicDataSecure(&publication_data, &pwr->e.guid, pwr->c.xqos, &pwr->security_info);
      ok = sc->access_control_context->check_remote_datawriter(sc->access_control_context, permissions_handle, (int)domain_id, &publication_data, &exception);
      q_omg_shallow_free_PublicationBuiltinTopicDataSecure(&publication_data);
      if (!ok)
      {
        if (!is_topic_discovery_protected(pp->permissions_handle, sc->access_control_context, publication_data.topic_name))
          EXCEPTION_ERROR(sc, &exception, "Access control does not allow remote writer "PGUIDFMT": %s", PGUID(pwr->e.guid));
        else
          DDS_Security_Exception_reset(&exception);
      }
    }
  }

  return ok;
}

static void send_reader_crypto_tokens(struct reader *rd, struct proxy_writer *pwr, DDS_Security_DatareaderCryptoHandle local_crypto, DDS_Security_DatawriterCryptoHandle remote_crypto)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(rd->c.pp);
  struct ddsi_domaingv *gv = rd->e.gv;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_DatawriterCryptoTokenSeq tokens = {0, 0, NULL};
  bool r;

  GVTRACE("send reader tokens "PGUIDFMT" to writer "PGUIDFMT"\n", PGUID(rd->e.guid), PGUID(pwr->e.guid));

  r = sc->crypto_context->crypto_key_exchange->create_local_datareader_crypto_tokens(sc->crypto_context->crypto_key_exchange, &tokens, local_crypto, remote_crypto, &exception);
  if (!r)
    EXCEPTION_ERROR(sc, &exception,"Failed to create local reader crypto tokens "PGUIDFMT" for remote writer "PGUIDFMT, PGUID(rd->e.guid), PGUID(pwr->e.guid));
  else
  {
    nn_dataholderseq_t tholder;

    q_omg_shallow_copyout_DataHolderSeq(&tholder, &tokens);
    write_crypto_reader_tokens(rd, pwr, &tholder);
    q_omg_shallow_free_nn_dataholderseq(&tholder);

    if (!sc->crypto_context->crypto_key_exchange->return_crypto_tokens(sc->crypto_context->crypto_key_exchange, &tokens, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return local reader crypto tokens "PGUIDFMT" for remote writer "PGUIDFMT, PGUID(rd->e.guid), PGUID(pwr->e.guid));
  }
}

static bool q_omg_security_register_remote_writer_match(struct proxy_writer *pwr, struct reader *rd, int64_t *crypto_handle)
{
  struct participant *pp = rd->c.pp;
  struct proxy_participant *proxypp = pwr->c.proxypp;
  struct ddsi_domaingv *gv = pp->e.gv;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct proxypp_pp_match *pm;
  struct security_entity_match *match;

  if (!q_omg_proxy_participant_is_secure(proxypp))
    return true;

  match = find_or_create_entity_match(gv->security_matches, &pwr->e.guid, &rd->e.guid);
  if (match->matched)
  {
    *crypto_handle = match->crypto_handle;
    return true;
  }

  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  pm = ddsrt_avl_lookup(&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp->sec_attr->crypto_handle);
  ddsrt_mutex_unlock(&proxypp->sec_attr->lock);

  if (!pm)
    return false;

  /* Generate writer crypto info. */
  match->crypto_handle = sc->crypto_context->crypto_key_factory->register_matched_remote_datawriter(
      sc->crypto_context->crypto_key_factory, rd->sec_attr->crypto_handle, pm->proxypp_crypto_handle, pm->shared_secret, &exception);

  if (match->crypto_handle == 0)
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to register remote writer "PGUIDFMT" with reader "PGUIDFMT, PGUID(pwr->e.guid), PGUID(rd->e.guid));
    return false;
  }

  *crypto_handle = match->crypto_handle;

  if (match->tokens)
  {
    if (sc->crypto_context->crypto_key_exchange->set_remote_datawriter_crypto_tokens(
        sc->crypto_context->crypto_key_exchange, rd->sec_attr->crypto_handle, match->crypto_handle, match->tokens, &exception))
    {
      match->matched = true;
      DDS_Security_DataHolderSeq_free(match->tokens);
      match->tokens = NULL;
      GVTRACE("match_remote_writer "PGUIDFMT" with reader "PGUIDFMT": tokens available\n", PGUID(pwr->e.guid), PGUID(rd->e.guid));
    }
    else
      EXCEPTION_ERROR(sc, &exception, "Failed to set remote writer crypto tokens "PGUIDFMT" --> "PGUIDFMT, PGUID(pwr->e.guid), PGUID(rd->e.guid));
  }

  if (rd->e.guid.entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_READER)
  {
    /* The builtin ParticipantVolatileSecure endpoints do not exchange tokens.
     * Simulate that we already got them. */
    match->matched = true;
    GVTRACE(" volatile secure reader: proxypp_crypto=%"PRId64" rd_crypto=%"PRId64" pwr_crypto=%"PRId64"\n", pm->proxypp_crypto_handle, rd->sec_attr->crypto_handle, match->crypto_handle);
  }
  else
  {
    /* For 'normal' endpoints, start exchanging tokens. */
    (void)send_reader_crypto_tokens(rd, pwr, rd->sec_attr->crypto_handle, match->crypto_handle);
  }

  return match->matched;
}

bool q_omg_security_match_remote_writer_enabled(struct reader *rd, struct proxy_writer *pwr, int64_t *crypto_handle)
{
  struct ddsi_domaingv *gv = rd->e.gv;
  nn_security_info_t info;

  if (!rd->sec_attr)
    return true;

  /*
   * Check if the security settings match by checking the attributes.
   *
   * The attributes will be 0 when security is not enabled for the related
   * federation or the security configuration told that this endpoint should
   * not be protected.
   *
   * This can mean that an unprotected endpoint of a secure federation can
   * connect to an endpoint of a non-secure federation. However, that will
   * be blocked by q_omg_security_check_remote_writer_permissions() if
   * q_omg_participant_allow_unauthenticated() returns FALSE there.
   */
  (void)q_omg_get_reader_security_info(rd, &info);
  if (!SECURITY_INFO_COMPATIBLE(pwr->security_info, info, NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID))
  {
    GVWARNING("match_remote_writer "PGUIDFMT" with reader "PGUIDFMT" security_attributes mismatch: 0x%08x.0x%08x - 0x%08x.0x%08x\n",
                PGUID(pwr->e.guid), PGUID(rd->e.guid),
                pwr->security_info.security_attributes, pwr->security_info.plugin_security_attributes,
                info.security_attributes, info.plugin_security_attributes);
    return false;
  }

  if ((!rd->sec_attr->attr.is_payload_protected ) && (!rd->sec_attr->attr.is_submessage_protected))
    return true;

  if (!q_omg_proxy_participant_is_secure(pwr->c.proxypp))
  {
    /* Remote proxy was downgraded to a non-secure participant,
     * but the local endpoint is protected. */
    return false;
  }

  /* We previously checked for attribute compatibility. That doesn't
   * mean equal, because compatibility depends on the valid flag.
   * Some products don't properly send the attributes, in which case
   * the valid flag is 0. To be able to support these product, assume
   * that the attributes are the same. If there is actually a mismatch,
   * communication will fail at a later moment anyway. */
  if (!SECURITY_ATTR_IS_VALID(pwr->security_info.security_attributes)) {
    pwr->security_info.security_attributes = info.security_attributes;
  }
  if (!SECURITY_ATTR_IS_VALID(pwr->security_info.plugin_security_attributes)) {
    pwr->security_info.plugin_security_attributes = info.plugin_security_attributes;
  }

  return q_omg_security_register_remote_writer_match(pwr, rd, crypto_handle);
}

void q_omg_security_deregister_remote_writer_match(const struct ddsi_domaingv *gv, const ddsi_guid_t *rd_guid, struct rd_pwr_match *m)
{
  struct dds_security_context *sc = gv->security_context;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct security_entity_match *match = NULL;

  if (m->crypto_handle != 0)
  {
    match = remove_entity_match(gv->security_matches, &m->pwr_guid, rd_guid);
    if (match)
    {
      assert(match->crypto_handle == m->crypto_handle);
      if (!sc->crypto_context->crypto_key_factory->unregister_datawriter(sc->crypto_context->crypto_key_factory, match->crypto_handle, &exception))
        EXCEPTION_ERROR(sc, &exception, "Failed to unregster remote writer "PGUIDFMT" for reader "PGUIDFMT, PGUID(m->pwr_guid), PGUID(*rd_guid));
      entity_match_free(match);
    }
  }
}

bool q_omg_security_check_remote_reader_permissions(const struct proxy_reader *prd, uint32_t domain_id, struct participant *pp)
{
  struct ddsi_domaingv *gv = pp->e.gv;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  bool ok = true;

  if (!sc)
    return true;

  if (!q_omg_proxy_participant_is_secure(prd->c.proxypp))
  {
    if (q_omg_participant_allow_unauthenticated(pp))
    {
      GVTRACE (" allow non-secure remote reader "PGUIDFMT, PGUID (prd->e.guid));
      return true;
    }
    else
    {
      GVWARNING("Non secure remote reader "PGUIDFMT" is not allowed.", PGUID(prd->e.guid));
      return false;
    }
  }

  if (SECURITY_INFO_IS_READ_PROTECTED(prd->security_info))
  {
    DDS_Security_PermissionsHandle permissions_handle;

    if ((permissions_handle = get_permissions_handle(pp, prd->c.proxypp)) != 0)
    {
      GVTRACE("Secure remote reader "PGUIDFMT" proxypp does not have permissions handle yet\n", PGUID(prd->e.guid));
      return false;
    }
    else
    {
      DDS_Security_SubscriptionBuiltinTopicDataSecure subscription_data;

      q_omg_shallow_copy_SubscriptionBuiltinTopicDataSecure(&subscription_data, &prd->e.guid, prd->c.xqos, &prd->security_info);
      ok = sc->access_control_context->check_remote_datareader(sc->access_control_context, permissions_handle, (int)domain_id, &subscription_data, false, &exception);
      q_omg_shallow_free_SubscriptionBuiltinTopicDataSecure(&subscription_data);
      if (!ok)
      {
        if (!is_topic_discovery_protected(pp->permissions_handle, sc->access_control_context, subscription_data.topic_name))
          EXCEPTION_ERROR(sc, &exception, "Access control does not allow remote reader "PGUIDFMT": %s", PGUID(prd->e.guid));
        else
          DDS_Security_Exception_reset(&exception);
      }
    }
  }

  return ok;
}

static void q_omg_get_proxy_endpoint_security_info(const struct entity_common *entity, nn_security_info_t *proxypp_sec_info, const ddsi_plist_t *plist, nn_security_info_t *info)
{
  const bool proxypp_info_available =
    (proxypp_sec_info->security_attributes != 0 || proxypp_sec_info->plugin_security_attributes != 0);

  /*
   * If Security info is present, use that.
   * Otherwise, use the specified values for the secure builtin endpoints.
   *      (Table 20 – EndpointSecurityAttributes for all "Builtin Security Endpoints")
   * Otherwise, reset.
   */
  if (plist->present & PP_ENDPOINT_SECURITY_INFO)
  {
    info->security_attributes = plist->endpoint_security_info.security_attributes;
    info->plugin_security_attributes = plist->endpoint_security_info.plugin_security_attributes;
  }
  else if (endpoint_is_DCPSParticipantSecure (&entity->guid)||
           endpoint_is_DCPSPublicationsSecure (&entity->guid) ||
           endpoint_is_DCPSSubscriptionsSecure (&entity->guid))
  {
    /* Discovery protection flags */
    info->plugin_security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    info->security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    if (proxypp_info_available)
    {
      if (proxypp_sec_info->security_attributes & NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_DISCOVERY_PROTECTED)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED;
      if (proxypp_sec_info->plugin_security_attributes & NN_PLUGIN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_DISCOVERY_ENCRYPTED)
        info->plugin_security_attributes |= NN_PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ENCRYPTED;
      if (proxypp_sec_info->plugin_security_attributes & NN_PLUGIN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_DISCOVERY_AUTHENTICATED)
        info->plugin_security_attributes |= NN_PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ORIGIN_AUTHENTICATED;
    }
    else
    {
      /* No participant info: assume hardcoded OpenSplice V6.10.0 values. */
      info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED;
      info->plugin_security_attributes |= NN_PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ENCRYPTED;
    }
  }
  else if (endpoint_is_DCPSParticipantMessageSecure (&entity->guid))
  {
    /* Liveliness protection flags */
    info->plugin_security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    info->security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    if (proxypp_info_available)
    {
      if (proxypp_sec_info->security_attributes & NN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_LIVELINESS_PROTECTED)
        info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED;
      if (proxypp_sec_info->plugin_security_attributes & NN_PLUGIN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_LIVELINESS_ENCRYPTED)
        info->plugin_security_attributes |= NN_PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ENCRYPTED;
      if (proxypp_sec_info->plugin_security_attributes & NN_PLUGIN_PARTICIPANT_SECURITY_ATTRIBUTES_FLAG_IS_LIVELINESS_AUTHENTICATED)
        info->plugin_security_attributes |= NN_PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ORIGIN_AUTHENTICATED;
    }
    else
    {
      /* No participant info: assume hardcoded OpenSplice V6.10.0 values. */
      info->security_attributes |= NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED;
      info->plugin_security_attributes |= NN_PLUGIN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_ENCRYPTED;
    }
  }
  else if (endpoint_is_DCPSParticipantStatelessMessage (&entity->guid))
  {
    info->security_attributes = NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID;
    info->plugin_security_attributes = 0;
  }
  else if (endpoint_is_DCPSParticipantVolatileMessageSecure (&entity->guid))
  {
    info->security_attributes =
      NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID | NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED;
    info->plugin_security_attributes = 0;
  }
  else
  {
    info->security_attributes = 0;
    info->plugin_security_attributes = 0;
  }
}

void q_omg_get_proxy_reader_security_info(struct proxy_reader *prd, const ddsi_plist_t *plist, nn_security_info_t *info)
{
  q_omg_get_proxy_endpoint_security_info(&(prd->e), &(prd->c.proxypp->security_info), plist, info);
}

void set_proxy_reader_security_info(struct proxy_reader *prd, const ddsi_plist_t *plist)
{
  assert (prd);
  q_omg_get_proxy_endpoint_security_info (&prd->e, &prd->c.proxypp->security_info, plist, &prd->c.security_info);
}


void q_omg_get_proxy_writer_security_info(struct proxy_writer *pwr, const ddsi_plist_t *plist, nn_security_info_t *info)
{
  q_omg_get_proxy_endpoint_security_info(&(pwr->e), &(pwr->c.proxypp->security_info), plist, info);
}

void set_proxy_writer_security_info(struct proxy_writer *pwr, const ddsi_plist_t *plist)
{
  assert (pwr);
  q_omg_get_proxy_endpoint_security_info (&pwr->e, &pwr->c.proxypp->security_info, plist, &pwr->c.security_info);
}

void q_omg_security_deregister_remote_reader_match(const struct ddsi_domaingv *gv, const ddsi_guid_t *wr_guid, struct wr_prd_match *m)
{
  struct dds_security_context *sc = gv->security_context;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct security_entity_match *match = NULL;

  if (m->crypto_handle)
  {
    match = remove_entity_match(gv->security_matches, &m->prd_guid, wr_guid);
    if (match)
    {
      assert(match->crypto_handle == m->crypto_handle);

      if (!sc->crypto_context->crypto_key_factory->unregister_datareader(sc->crypto_context->crypto_key_factory, match->crypto_handle, &exception))
        EXCEPTION_ERROR(sc, &exception, "Failed to unregister remote reader "PGUIDFMT" for writer "PGUIDFMT, PGUID(m->prd_guid), PGUID(*wr_guid));
      entity_match_free(match);
    }
  }
}

static void send_writer_crypto_tokens(struct writer *wr, struct proxy_reader *prd, DDS_Security_DatawriterCryptoHandle local_crypto, DDS_Security_DatareaderCryptoHandle remote_crypto)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(wr->c.pp);
  struct ddsi_domaingv *gv = wr->e.gv;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_DatawriterCryptoTokenSeq tokens = {0, 0, NULL};
  bool r;

  GVTRACE("send writer tokens "PGUIDFMT" to reader "PGUIDFMT"\n", PGUID(wr->e.guid), PGUID(prd->e.guid));

  r = sc->crypto_context->crypto_key_exchange->create_local_datawriter_crypto_tokens(sc->crypto_context->crypto_key_exchange, &tokens, local_crypto, remote_crypto, &exception);
  if (!r)
    EXCEPTION_ERROR(sc, &exception,"Failed to create local writer crypto tokens "PGUIDFMT" for remote reader "PGUIDFMT, PGUID(wr->e.guid), PGUID(prd->e.guid));
  else
  {
    nn_dataholderseq_t tholder;

    q_omg_shallow_copyout_DataHolderSeq(&tholder, &tokens);
    write_crypto_writer_tokens(wr, prd, &tholder);
    q_omg_shallow_free_nn_dataholderseq(&tholder);

    if (!sc->crypto_context->crypto_key_exchange->return_crypto_tokens(sc->crypto_context->crypto_key_exchange, &tokens, &exception))
      EXCEPTION_ERROR(sc, &exception, "Failed to return local writer crypto tokens "PGUIDFMT" for remote reader "PGUIDFMT, PGUID(wr->e.guid), PGUID(prd->e.guid));
  }
}

static bool q_omg_security_register_remote_reader_match(struct proxy_reader *prd, struct writer *wr, int64_t *crypto_handle)
{
  struct participant *pp = wr->c.pp;
  struct proxy_participant *proxypp = prd->c.proxypp;
  struct ddsi_domaingv *gv = pp->e.gv;
  struct dds_security_context *sc = q_omg_security_get_secure_context(pp);
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct proxypp_pp_match *pm;
  struct security_entity_match *match;

  *crypto_handle = 0;

  if (!q_omg_proxy_participant_is_secure(proxypp))
    return true;

   match = find_or_create_entity_match(gv->security_matches, &prd->e.guid, &wr->e.guid);
   if (match->matched)
   {
     *crypto_handle = match->crypto_handle;
     return true;
   }

  ddsrt_mutex_lock(&proxypp->sec_attr->lock);
  pm = ddsrt_avl_lookup(&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp->sec_attr->crypto_handle);
  ddsrt_mutex_unlock(&proxypp->sec_attr->lock);
  if (!pm)
    return false;

  /* Generate writer crypto info. */
  match->crypto_handle = sc->crypto_context->crypto_key_factory->register_matched_remote_datareader(
      sc->crypto_context->crypto_key_factory, wr->sec_attr->crypto_handle, pm->proxypp_crypto_handle, pm->shared_secret, false, &exception);

  if (match->crypto_handle == 0)
  {
    EXCEPTION_ERROR(sc, &exception, "Failed to register remote reader "PGUIDFMT" with writer "PGUIDFMT, PGUID(prd->e.guid), PGUID(wr->e.guid));
    return false;
  }

  *crypto_handle = match->crypto_handle;

  if (match->tokens)
  {
    if (sc->crypto_context->crypto_key_exchange->set_remote_datareader_crypto_tokens(
        sc->crypto_context->crypto_key_exchange, wr->sec_attr->crypto_handle, match->crypto_handle, match->tokens, &exception))
    {
      match->matched = true;
      DDS_Security_DataHolderSeq_free(match->tokens);
      match->tokens = NULL;;
      GVTRACE("match_remote_reader "PGUIDFMT" with writer "PGUIDFMT": tokens available\n", PGUID(prd->e.guid), PGUID(wr->e.guid));
    }
    else
      EXCEPTION_ERROR(sc, &exception, "Failed to set remote reader crypto tokens "PGUIDFMT" --> "PGUIDFMT, PGUID(prd->e.guid), PGUID(wr->e.guid));
  }

  if (wr->e.guid.entityid.u == NN_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER)
  {
    /* The builtin ParticipantVolatileSecure endpoints do not exchange tokens.
     * Simulate that we already got them. */
    match->matched = true;
    GVTRACE(" volatile secure writer: proxypp_crypto=%"PRId64" wr_crypto=%"PRId64" prd_crypto=%"PRId64"\n", pm->proxypp_crypto_handle, wr->sec_attr->crypto_handle, match->crypto_handle);
  }
  else
  {
    /* For 'normal' endpoints, start exchanging tokens. */
    (void)send_writer_crypto_tokens(wr, prd, wr->sec_attr->crypto_handle, match->crypto_handle);
  }

  return match->matched;
}


bool q_omg_security_match_remote_reader_enabled(struct writer *wr, struct proxy_reader *prd, int64_t *crypto_handle)
{
  struct ddsi_domaingv *gv = wr->e.gv;
  nn_security_info_t info;

  if (!wr->sec_attr)
    return true;

  if (!q_omg_proxy_participant_is_secure(prd->c.proxypp))
    return false;

  /*
   * Check if the security settings match by checking the attributes.
   *
   * The attributes will be 0 when security is not enabled for the related
   * federation or the security configuration told that this endpoint should
   * not be protected.
   *
   * This can mean that an unprotected endpoint of a secure federation can
   * connect to an endpoint of a non-secure federation. However, that will
   * be blocked by q_omg_security_check_remote_reader_permissions() if
   * q_omg_participant_allow_unauthenticated() returns FALSE there.
   */
  (void)q_omg_get_writer_security_info(wr, &info);
  if (!SECURITY_INFO_COMPATIBLE(prd->security_info, info, NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID))
  {
    GVWARNING("match_remote_reader "PGUIDFMT" with writer "PGUIDFMT" security_attributes mismatch: 0x%08x.0x%08x - 0x%08x.0x%08x\n",
        PGUID(prd->e.guid), PGUID(wr->e.guid),
        prd->security_info.security_attributes, prd->security_info.plugin_security_attributes,
        info.security_attributes, info.plugin_security_attributes);
    return false;
  }

  if (!wr->sec_attr->attr.is_submessage_protected  && !wr->sec_attr->attr.is_payload_protected)
    return true;

  if (!q_omg_proxy_participant_is_secure(prd->c.proxypp))
  {
    /* Remote proxy was downgraded to a non-secure participant,
     * but the local endpoint is protected. */
    return false;
  }

  /* We previously checked for attribute compatibility. That doesn't
   * mean equal, because compatibility depends on the valid flag.
   * Some products don't properly send the attributes, in which case
   * the valid flag is 0. To be able to support these product, assume
   * that the attributes are the same. If there is actually a mismatch,
   * communication will fail at a later moment anyway. */
  if (!SECURITY_ATTR_IS_VALID(prd->security_info.security_attributes)) {
    prd->security_info.security_attributes = info.security_attributes;
  }
  if (!SECURITY_ATTR_IS_VALID(prd->security_info.plugin_security_attributes)) {
    prd->security_info.plugin_security_attributes = info.plugin_security_attributes;
  }

  return q_omg_security_register_remote_reader_match(prd, wr, crypto_handle);
}

void q_omg_security_set_remote_writer_crypto_tokens(struct reader *rd, const ddsi_guid_t *pwr_guid, const nn_dataholderseq_t *tokens)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(rd->c.pp);
  struct ddsi_domaingv *gv = rd->e.gv;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct security_entity_match *match;
  struct proxy_writer *pwr;

  if (!sc)
     return;

  match = find_or_create_entity_match(gv->security_matches, pwr_guid, &rd->e.guid);
  if (match->matched)
    return;

  pwr = entidx_lookup_proxy_writer_guid(gv->entity_index, pwr_guid);
  if (pwr && q_omg_proxy_participant_is_secure(pwr->c.proxypp))
  {
    DDS_Security_DatawriterCryptoTokenSeq *tseq;

    tseq = DDS_Security_DataHolderSeq_alloc();
    q_omg_copyin_DataHolderSeq(tseq, tokens);

    if (match->crypto_handle != 0)
    {
      if (sc->crypto_context->crypto_key_exchange->set_remote_datawriter_crypto_tokens(sc->crypto_context->crypto_key_exchange, rd->sec_attr->crypto_handle, match->crypto_handle, tseq, &exception))
      {
        GVTRACE("set_remote_writer_crypto_tokens "PGUIDFMT" with reader "PGUIDFMT"\n", PGUID(pwr->e.guid), PGUID(rd->e.guid));
        match->matched = true;
        DDS_Security_DataHolderSeq_free(tseq);
        connect_reader_with_proxy_writer_secure(rd, pwr, now_mt (), match->crypto_handle);
      }
      else
      {
        EXCEPTION_ERROR(sc, &exception, "Failed to set remote writer crypto tokens "PGUIDFMT" for reader "PGUIDFMT, PGUID(pwr->e.guid), PGUID(rd->e.guid));
      }
    }
    else
    {
      GVTRACE("remember writer tokens src("PGUIDFMT") dst("PGUIDFMT")\n", PGUID(pwr->e.guid), PGUID(rd->e.guid));
      match->tokens = tseq;
    }
    notify_handshake_recv_token(rd->c.pp, pwr->c.proxypp);
  }
}

void q_omg_security_set_remote_reader_crypto_tokens(struct writer *wr, const ddsi_guid_t *prd_guid, const nn_dataholderseq_t *tokens)
{
  struct dds_security_context *sc = q_omg_security_get_secure_context(wr->c.pp);
  struct ddsi_domaingv *gv = wr->e.gv;
  DDS_Security_SecurityException exception = DDS_SECURITY_EXCEPTION_INIT;
  struct security_entity_match *match;
  struct proxy_reader *prd;

  if (!sc)
     return;

  match = find_or_create_entity_match(gv->security_matches, prd_guid, &wr->e.guid);
  if (match->matched)
    return;

  prd = entidx_lookup_proxy_reader_guid(gv->entity_index, prd_guid);
  if (prd) {
    DDS_Security_DatawriterCryptoTokenSeq *tseq;

    tseq = DDS_Security_DataHolderSeq_alloc();
    q_omg_copyin_DataHolderSeq(tseq, tokens);

    if (match->crypto_handle != 0)
    {
      if (sc->crypto_context->crypto_key_exchange->set_remote_datareader_crypto_tokens(sc->crypto_context->crypto_key_exchange, wr->sec_attr->crypto_handle, match->crypto_handle, tseq, &exception))
      {
        GVTRACE("set_remote_reader_crypto_tokens "PGUIDFMT" with writer "PGUIDFMT"\n", PGUID(prd->e.guid), PGUID(wr->e.guid));
        match->matched = true;
        DDS_Security_DataHolderSeq_free(tseq);
        connect_writer_with_proxy_reader_secure(wr, prd, now_mt (), match->crypto_handle);
      }
      else
      {
        EXCEPTION_ERROR(sc, &exception, "Failed to set remote reader crypto tokens "PGUIDFMT" for writer "PGUIDFMT, PGUID(prd->e.guid), PGUID(wr->e.guid));
      }
    }
    else
    {
      GVTRACE("remember reader tokens src("PGUIDFMT") dst("PGUIDFMT")\n", PGUID(prd->e.guid), PGUID(wr->e.guid));
      match->tokens = tseq;
    }
    notify_handshake_recv_token(wr->c.pp, prd->c.proxypp);
  }
}

bool q_omg_reader_is_discovery_protected(const struct reader *rd)
{
  assert (rd != NULL);
  return rd->sec_attr != NULL && rd->sec_attr->attr.is_discovery_protected;
}

static bool q_omg_security_encode_datareader_submessage(struct reader *rd, const ddsi_guid_prefix_t *dst_prefix, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  struct rd_pwr_match *m;
  ddsrt_avl_iter_t it;
  DDS_Security_DatareaderCryptoHandleSeq hdls = { 0, 0, NULL };
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer;
  bool result = false;
  int32_t idx = 0;

  assert (rd);
  assert (src_len <= UINT32_MAX);
  assert (src_buf);
  assert (dst_len);
  assert (dst_buf);
  assert (rd->sec_attr);
  assert (q_omg_reader_is_submessage_protected (rd));

  const struct ddsi_domaingv *gv = rd->e.gv;
  const struct dds_security_context *sc = q_omg_security_get_secure_context (rd->c.pp);
  assert (sc);

  GVTRACE (" encode_datareader_submessage "PGUIDFMT" %s/%s", PGUID (rd->e.guid), get_reader_topic_name (rd), rd->topic ? rd->topic->type_name : "(null)");
  // FIXME: print_buf(src_buf, src_len, "q_omg_security_encode_datareader_submessage(SOURCE)");

  ddsrt_mutex_lock (&rd->e.lock);
  hdls._buffer = DDS_Security_DatawriterCryptoHandleSeq_allocbuf (rd->num_writers);
  hdls._maximum = rd->num_writers;
  for (m = ddsrt_avl_iter_first (&rd_writers_treedef, &rd->writers, &it); m; m = ddsrt_avl_iter_next (&it))
  {
    if (m->crypto_handle && (!dst_prefix || guid_prefix_eq (&m->pwr_guid.prefix, dst_prefix)))
      hdls._buffer[idx++] = m->crypto_handle;
  }
  ddsrt_mutex_unlock (&rd->e.lock);

  if ((hdls._length = (DDS_Security_unsigned_long) idx) == 0)
  {
    GVTRACE ("Submsg encoding failed for datareader "PGUIDFMT" %s/%s: no matching writers\n", PGUID (rd->e.guid),
        get_reader_topic_name (rd), rd->topic ? rd->topic->type_name : "(null)");
    goto err_enc_drd_subm;
  }

  memset (&encoded_buffer, 0, sizeof (encoded_buffer));
  plain_buffer._buffer = (DDS_Security_octet*) src_buf;
  plain_buffer._length = (uint32_t) src_len;
  plain_buffer._maximum = (uint32_t) src_len;

  if (!(result = sc->crypto_context->crypto_transform->encode_datareader_submessage (
      sc->crypto_context->crypto_transform, &encoded_buffer, &plain_buffer, rd->sec_attr->crypto_handle, &hdls, &ex)))
  {
    GVWARNING ("Submsg encoding failed for datareader "PGUIDFMT" %s/%s: %s", PGUID (rd->e.guid), get_reader_topic_name (rd),
        rd->topic ? rd->topic->type_name : "(null)", ex.message ? ex.message : "Unknown error");
    GVTRACE ("\n");
    DDS_Security_Exception_reset (&ex);
    goto err_enc_drd_subm;
  }
  assert (encoded_buffer._buffer);
  *dst_buf = encoded_buffer._buffer;
  *dst_len = encoded_buffer._length;
  // FIXME: print_buf (*dst_buf, *dst_len, "q_omg_security_encode_datareader_submessage(DEST)");
  goto end_enc_drd_subm;

err_enc_drd_subm:
  *dst_buf = NULL;
  *dst_len = 0;

end_enc_drd_subm:
  DDS_Security_DatawriterCryptoHandleSeq_freebuf (&hdls);
  return result;
}

static bool q_omg_security_encode_datawriter_submessage (struct writer *wr, const ddsi_guid_prefix_t *dst_prefix, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  struct wr_prd_match *m;
  ddsrt_avl_iter_t it;
  DDS_Security_DatareaderCryptoHandleSeq hdls = { 0, 0, NULL };
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer;
  bool result = false;
  int32_t idx = 0;

  assert (wr);
  assert (src_len <= UINT32_MAX);
  assert (src_buf);
  assert (dst_len);
  assert (dst_buf);
  assert (wr->sec_attr);
  assert (q_omg_writer_is_submessage_protected (wr));
  ASSERT_MUTEX_HELD (wr->e.lock);

  const struct ddsi_domaingv *gv = wr->e.gv;
  const struct dds_security_context *sc = q_omg_security_get_secure_context (wr->c.pp);
  assert (sc);

  GVTRACE (" encode_datawriter_submessage "PGUIDFMT" %s/%s", PGUID (wr->e.guid), get_writer_topic_name (wr), wr->topic ? wr->topic->type_name : "(null)");

  // FIXME: print_buf(src_buf, src_len, "q_omg_security_encode_datawriter_submessage(SOURCE)");

  hdls._buffer = DDS_Security_DatareaderCryptoHandleSeq_allocbuf (wr->num_readers);
  hdls._maximum = wr->num_readers;
  for (m = ddsrt_avl_iter_first (&wr_readers_treedef, &wr->readers, &it); m; m = ddsrt_avl_iter_next (&it))
  {
    if (m->crypto_handle && (!dst_prefix || guid_prefix_eq (&m->prd_guid.prefix, dst_prefix)))
      hdls._buffer[idx++] = m->crypto_handle;
  }

  if ((hdls._length = (DDS_Security_unsigned_long) idx) == 0)
  {
    GVTRACE ("Submsg encoding failed for datawriter "PGUIDFMT" %s/%s: no matching readers\n", PGUID (wr->e.guid),
        get_writer_topic_name (wr), wr->topic ? wr->topic->type_name : "(null)");
    goto err_enc_dwr_subm;
  }

  memset (&encoded_buffer, 0, sizeof (encoded_buffer));
  plain_buffer._buffer = (DDS_Security_octet*) src_buf;
  plain_buffer._length = (uint32_t) src_len;
  plain_buffer._maximum = (uint32_t) src_len;
  result = true;
  idx = 0;
  while (result && idx < (int32_t)hdls._length)
  {
    /* If the plugin thinks a new call is unnecessary, the index will be set to the size of the hdls sequence. */
    result = sc->crypto_context->crypto_transform->encode_datawriter_submessage (sc->crypto_context->crypto_transform,
        &encoded_buffer, &plain_buffer, wr->sec_attr->crypto_handle, &hdls, &idx, &ex);

    /* With a possible second call to encode, the plain buffer should be NULL. */
    plain_buffer._buffer = NULL;
    plain_buffer._length = 0;
    plain_buffer._maximum = 0;
  }

  if (!result)
  {
    GVWARNING ("Submsg encoding failed for datawriter "PGUIDFMT" %s/%s: %s", PGUID (wr->e.guid), get_writer_topic_name (wr),
        wr->topic ? wr->topic->type_name : "(null)", ex.message ? ex.message : "Unknown error");
    GVTRACE ("\n");
    DDS_Security_Exception_reset (&ex);
    goto err_enc_dwr_subm;
  }

  assert (encoded_buffer._buffer);
  *dst_buf = encoded_buffer._buffer;
  *dst_len = encoded_buffer._length;
  // FIXME: print_buf (*dst_buf, *dst_len, "q_omg_security_encode_datawriter_submessage(DEST)");
  goto end_enc_dwr_subm;

err_enc_dwr_subm:
  *dst_buf = NULL;
  *dst_len = 0;

end_enc_dwr_subm:
  DDS_Security_DatareaderCryptoHandleSeq_freebuf (&hdls);
  return result;
}

static bool q_omg_security_decode_submessage (const struct ddsi_domaingv *gv, const ddsi_guid_prefix_t * const src_prefix, const ddsi_guid_prefix_t * const dst_prefix, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  struct dds_security_context *sc = NULL;
  DDS_Security_SecureSubmessageCategory_t cat = 0;
  DDS_Security_DatawriterCryptoHandle pp_crypto_hdl = DDS_SECURITY_HANDLE_NIL;
  DDS_Security_DatawriterCryptoHandle proxypp_crypto_hdl = DDS_SECURITY_HANDLE_NIL;
  DDS_Security_DatawriterCryptoHandle send_crypto_hdl = DDS_SECURITY_HANDLE_NIL;
  DDS_Security_DatareaderCryptoHandle recv_crypto_hdl = DDS_SECURITY_HANDLE_NIL;
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer;
  struct proxypp_pp_match *m;
  struct participant *pp = NULL;
  struct proxy_participant *proxypp;
  struct ddsi_guid proxypp_guid, pp_guid;
  bool result;

  assert (src_len <= UINT32_MAX);
  assert (src_buf);
  assert (dst_len);
  assert (dst_buf);

  // FIXME: print_buf(src_buf, src_len, "q_omg_security_decode_submessage(SOURCE)");

  proxypp_guid.prefix = *src_prefix;
  proxypp_guid.entityid.u = NN_ENTITYID_PARTICIPANT;
  if (!(proxypp = entidx_lookup_proxy_participant_guid (gv->entity_index, &proxypp_guid)))
  {
    GVTRACE (" Unknown remote participant "PGUIDFMT" for decoding submsg\n", PGUID (proxypp_guid));
    return false;
  }
  if (!proxypp->sec_attr)
  {
    GVTRACE (" Remote participant "PGUIDFMT" not secure for decoding submsg\n", PGUID (proxypp_guid));
    return false;
  }

  if (!dst_prefix || guid_prefix_zero (dst_prefix))
  {
    ddsrt_mutex_lock (&proxypp->sec_attr->lock);
    m = ddsrt_avl_root(&proxypp_pp_treedef, &proxypp->sec_attr->participants);
    proxypp_crypto_hdl = m->proxypp_crypto_handle;
    memset(&pp_guid, 0, sizeof(pp_guid));
    ddsrt_mutex_unlock (&proxypp->sec_attr->lock);
    sc = q_omg_security_get_secure_context_from_proxypp (proxypp);
  }
  else
  {
    pp_guid.prefix = *dst_prefix;
    pp_guid.entityid.u = NN_ENTITYID_PARTICIPANT;
    if (!(pp = entidx_lookup_participant_guid (gv->entity_index, &pp_guid)))
      return false;

    sc = q_omg_security_get_secure_context (pp);
    pp_crypto_hdl = pp->sec_attr->crypto_handle;
    ddsrt_mutex_lock (&proxypp->sec_attr->lock);
    m = ddsrt_avl_lookup(&proxypp_pp_treedef, &proxypp->sec_attr->participants, &pp_crypto_hdl);
    if (m)
      proxypp_crypto_hdl = m->proxypp_crypto_handle;
    ddsrt_mutex_unlock (&proxypp->sec_attr->lock);
  }

  if (proxypp_crypto_hdl == DDS_SECURITY_HANDLE_NIL)
  {
    GVTRACE (" Remote participant "PGUIDFMT" not matched yet for decoding submsg\n", PGUID (proxypp_guid));
    return false;
  }

  GVTRACE(" decode: pp_crypto=%"PRId64" proxypp_crypto=%"PRId64"\n", pp ? pp->sec_attr->crypto_handle:0, proxypp_crypto_hdl);
  /* Prepare buffers. */
  memset (&plain_buffer, 0, sizeof (plain_buffer));
  encoded_buffer._buffer = (DDS_Security_octet*) src_buf;
  encoded_buffer._length = (uint32_t) src_len;
  encoded_buffer._maximum = (uint32_t) src_len;

  /* Determine how the RTPS sub-message was encoded. */
  assert (sc);
  result = sc->crypto_context->crypto_transform->preprocess_secure_submsg (sc->crypto_context->crypto_transform, &recv_crypto_hdl, &send_crypto_hdl,
      &cat, &encoded_buffer, pp_crypto_hdl, proxypp_crypto_hdl, &ex);
  GVTRACE ("decode_submessage: pp("PGUIDFMT") proxypp("PGUIDFMT"), cat(%d)", PGUID (pp_guid), PGUID (proxypp_guid), (int) cat);
  if (!result)
  {
    GVTRACE ("Pre-process submsg failed: %s\n", ex.message ? ex.message : "Unknown error");
    DDS_Security_Exception_reset (&ex);
    return false;
  }

  if (cat == DDS_SECURITY_INFO_SUBMESSAGE)
  {
    /* FIXME: Is DDS_SECURITY_INFO_SUBMESSAGE even possible when there's a SMID_SEC_PREFIX?
      * This function is only called when there is a prefix. If it is possible, then we might
      * have a problem because the further parsing expects a new buffer (without the security
      * sub-messages). For now, consider this an error. */
    GVWARNING ("Pre-process submsg returned DDS_SECURITY_INFO_SUBMESSAGE, which is unexpected with SMID_SEC_PREFIX\n");
    *dst_buf = NULL;
    *dst_len = 0;
    return false;
  }
  if (cat != DDS_SECURITY_DATAREADER_SUBMESSAGE && cat != DDS_SECURITY_DATAWRITER_SUBMESSAGE)
  {
    GVWARNING ("Pre-process submsg failed from datawriter: returned unknown cat %d\n", (int) cat);
    return false;
  }

  switch (cat)
  {
  case DDS_SECURITY_DATAWRITER_SUBMESSAGE:
    result = sc->crypto_context->crypto_transform->decode_datawriter_submessage(sc->crypto_context->crypto_transform, &plain_buffer, &encoded_buffer, send_crypto_hdl, recv_crypto_hdl, &ex);
    break;
  case DDS_SECURITY_DATAREADER_SUBMESSAGE:
    result = sc->crypto_context->crypto_transform->decode_datareader_submessage(sc->crypto_context->crypto_transform, &plain_buffer, &encoded_buffer, recv_crypto_hdl, send_crypto_hdl, &ex);
    break;
  case DDS_SECURITY_INFO_SUBMESSAGE:
    /* No decoding needed.
     * TODO: Is DDS_SECURITY_INFO_SUBMESSAGE even possible when there's a SMID_SEC_PREFIX?
     *
     * This function is only called when there is a prefix. If it is possible,
     * then I might have a problem because the further parsing expects a new
     * buffer (without the security sub-messages).
     *
     */
    result = true;
    break;
  default:
    result = false;
    break;
  }

  if (!result)
  {
    GVWARNING ("Submsg decoding failed: %s\n", ex.message ? ex.message : "Unknown error");
    DDS_Security_Exception_reset (&ex);
    *dst_buf = NULL;
    *dst_len = 0;
    return false;
  }

  assert (plain_buffer._buffer);
  *dst_buf = plain_buffer._buffer;
  *dst_len = plain_buffer._length;
  // FIXME: print_buf(*dst_buf, *dst_len, "q_omg_security_decode_submessage(DEST-DATAWRITER)");
  return true;
}

static bool q_omg_security_encode_serialized_payload (const struct writer *wr, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_OctetSeq extra_inline_qos;
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer;

  assert (wr);
  assert (src_buf);
  assert (src_len <= UINT32_MAX);
  assert (dst_buf);
  assert (dst_len);
  assert (wr->sec_attr);
  assert (q_omg_writer_is_payload_protected (wr));

  const struct ddsi_domaingv *gv = wr->e.gv;
  const struct dds_security_context *sc = q_omg_security_get_secure_context (wr->c.pp);
  assert (sc);

  // FIXME: print_buf(src_buf, src_len, "q_omg_security_encode_serialized_payload(SOURCE)");

  GVTRACE (" encode_payload "PGUIDFMT" %s/%s\n", PGUID (wr->e.guid), wr->topic ? wr->topic->name : "(null)", wr->topic ? wr->topic->type_name : "(null)");

  memset (&extra_inline_qos, 0, sizeof (extra_inline_qos));
  memset (&encoded_buffer, 0, sizeof (encoded_buffer));
  plain_buffer._buffer = (DDS_Security_octet *) src_buf;
  plain_buffer._length = (uint32_t) src_len;
  plain_buffer._maximum = (uint32_t) src_len;

  if (!sc->crypto_context->crypto_transform->encode_serialized_payload (sc->crypto_context->crypto_transform,
      &encoded_buffer, &extra_inline_qos, &plain_buffer, wr->sec_attr->crypto_handle, &ex))
  {
    GVERROR ("Payload encoding failed for datawriter "PGUIDFMT": %s\n", PGUID (wr->e.guid), ex.message ? ex.message : "Unknown error");
    DDS_Security_Exception_reset (&ex);
    *dst_buf = NULL;
    *dst_len = 0;
    return false;
  }

  *dst_buf = encoded_buffer._buffer;
  *dst_len = encoded_buffer._length;
  // FIXME: print_buf(*dst_buf, *dst_len, "q_omg_security_encode_serialized_payload(DEST)");

  return true;
}

static bool q_omg_security_decode_serialized_payload (struct proxy_writer *pwr, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_OctetSeq extra_inline_qos;
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer;
  struct pwr_rd_match *pwr_rd_match;
  struct reader *rd;
  ddsrt_avl_iter_t it;

  assert (pwr);
  assert (src_buf);
  assert (src_len <= UINT32_MAX);
  assert (dst_buf);
  assert (dst_len);

  const struct ddsi_domaingv *gv = pwr->e.gv;
  const struct dds_security_context *sc = q_omg_security_get_secure_context_from_proxypp (pwr->c.proxypp);
  assert (sc);

  // FIXME: print_buf(src_buf, src_len, "q_omg_security_decode_serialized_payload(SOURCE)");

  *dst_buf = NULL;
  *dst_len = 0;
  GVTRACE ("decode_payload "PGUIDFMT"", PGUID (pwr->e.guid));

  /* Only one reader is enough to decrypt the data, so use only the first match. */
  ddsrt_mutex_lock (&pwr->e.lock);
  pwr_rd_match = ddsrt_avl_iter_first (&pwr_readers_treedef, &pwr->readers, &it);
  ddsrt_mutex_unlock (&pwr->e.lock);
  if (!pwr_rd_match)
  {
    GVWARNING ("Payload decoding failed for from remote datawriter "PGUIDFMT": no local reader\n", PGUID (pwr->e.guid));
    return false;
  }
  if (!pwr_rd_match->crypto_handle)
  {
    GVWARNING ("Payload decoding from datawriter "PGUIDFMT": no crypto handle\n", PGUID (pwr->e.guid));
    return false;
  }
  if (!(rd = entidx_lookup_reader_guid (gv->entity_index, &pwr_rd_match->rd_guid)))
  {
    GVWARNING ("No datareader "PGUIDFMT" for decoding data from datawriter "PGUIDFMT"", PGUID (pwr_rd_match->rd_guid), PGUID (pwr->e.guid));
    return false;
  }

  memset (&extra_inline_qos, 0, sizeof (extra_inline_qos));
  memset (&plain_buffer, 0, sizeof (plain_buffer));
  encoded_buffer._buffer  = (DDS_Security_octet *) src_buf;
  encoded_buffer._length  = (uint32_t) src_len;
  encoded_buffer._maximum = (uint32_t) src_len;
  if (!sc->crypto_context->crypto_transform->decode_serialized_payload (sc->crypto_context->crypto_transform,
      &plain_buffer, &encoded_buffer, &extra_inline_qos, rd->sec_attr->crypto_handle, pwr_rd_match->crypto_handle, &ex))
  {
    GVWARNING ("Payload decoding failed for datareader "PGUIDFMT" from datawriter "PGUIDFMT": %s\n", PGUID (pwr_rd_match->rd_guid), PGUID (pwr->e.guid), ex.message ? ex.message : "Unknown error");
    DDS_Security_Exception_reset (&ex);
    return false;
  }
  *dst_buf = plain_buffer._buffer;
  *dst_len = plain_buffer._length;
  // FIXME: print_buf(*dst_buf, *dst_len, "q_omg_security_decode_serialized_payload(DEST)");
  return true;
}

bool q_omg_security_encode_rtps_message (const struct ddsi_domaingv *gv, int64_t src_handle, const ddsi_guid_t *src_guid, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len, int64_t dst_handle)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  DDS_Security_ParticipantCryptoHandleSeq hdls = { 0, 0, NULL };
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer;
  struct participant *pp;
  bool result = false;
  int32_t idx = 0;

  assert (src_buf);
  assert (src_len <= UINT32_MAX);
  assert (dst_buf);
  assert (dst_len);

  if (!(pp = entidx_lookup_participant_guid (gv->entity_index, src_guid)))
  {
//    GVWARNING ("Unknown local participant "PGUIDFMT" for encoding rtps message\n", PGUID (*src_guid));
    return false;
  }

  const struct dds_security_context *sc = q_omg_security_get_secure_context (pp);
  assert (sc);

  GVTRACE (" ] encode_rtps_message ["PGUIDFMT, PGUID (*src_guid));

  /* When not send to a particular remote participant the message will be send to all known remote participants. */
  if (dst_handle == DDS_SECURITY_HANDLE_NIL)
  {
    /* FIXME:
    os_rwlockRead(&q_omg_remote_par_crypto_hdls.rwlock);
    hdls._length = q_omg_remote_par_crypto_hdls.num_handles;
    hdls._maximum = q_omg_remote_par_crypto_hdls.max_handles;
    hdls._buffer = (DDS_Security_long_long *)q_omg_remote_par_crypto_hdls.handles;
    */
  }
  else
  {
    hdls._buffer = (DDS_Security_long_long *) &dst_handle;
    hdls._length = hdls._maximum = 1;
  }

  if (hdls._length > 0)
  {
    memset (&encoded_buffer, 0, sizeof (encoded_buffer));
    plain_buffer._buffer = (DDS_Security_octet *) src_buf;
    plain_buffer._length = (uint32_t) src_len;
    plain_buffer._maximum = (uint32_t) src_len;

    result = true;
    idx = 0;
    while (result && idx < (int32_t) hdls._length)
    {
      /* If the plugin thinks a new call is unnecessary, the index will be set to the size of the hdls sequence. */
      result = sc->crypto_context->crypto_transform->encode_rtps_message (sc->crypto_context->crypto_transform,
          &encoded_buffer, &plain_buffer, src_handle, &hdls, &idx, &ex);

      /* With a possible second call to encode, the plain buffer should be NULL. */
      plain_buffer._buffer = NULL;
      plain_buffer._length = 0;
      plain_buffer._maximum = 0;
    }

    if (!result)
    {
      GVTRACE ("]\n");
      GVERROR ("encoding rtps message for participant "PGUIDFMT" failed: %s", PGUID (*src_guid), ex.message ? ex.message : "Unknown error");
      GVTRACE ("[");
      DDS_Security_Exception_reset (&ex);
      *dst_buf = NULL;
      *dst_len = 0;
    }
    else
    {
      assert (encoded_buffer._buffer);
      *dst_buf = encoded_buffer._buffer;
      *dst_len = encoded_buffer._length;
    }
  }

  if (dst_handle == DDS_SECURITY_HANDLE_NIL)
  {
    // FIXME os_rwlockUnlock(&q_omg_remote_par_crypto_hdls.rwlock);
  }

  return result;
}

static bool q_omg_security_decode_rtps_message (struct proxy_participant *proxypp, const unsigned char *src_buf, size_t src_len, unsigned char **dst_buf, size_t *dst_len)
{
  DDS_Security_SecurityException ex = DDS_SECURITY_EXCEPTION_INIT;
  struct dds_security_context *sc;
  DDS_Security_OctetSeq encoded_buffer;
  DDS_Security_OctetSeq plain_buffer = {0, 0, NULL};
  ddsrt_avl_iter_t it;

  assert (proxypp);
  assert (src_buf);
  assert (src_len <= UINT32_MAX);
  assert (dst_buf);
  assert (dst_len);

  const struct ddsi_domaingv *gv = proxypp->e.gv;
  GVTRACE ("decode_rtps_message from "PGUIDFMT"\n", PGUID (proxypp->e.guid));

  *dst_buf = NULL;
  *dst_len = 0;
  encoded_buffer._buffer = (DDS_Security_octet *) src_buf;
  encoded_buffer._length = (uint32_t) src_len;
  encoded_buffer._maximum = (uint32_t) src_len;

  ddsrt_mutex_lock (&proxypp->sec_attr->lock);
  for (struct proxypp_pp_match *pm = ddsrt_avl_iter_first (&proxypp_pp_treedef, &proxypp->sec_attr->participants, &it); pm; pm = ddsrt_avl_iter_next (&it))
  {
    sc = q_omg_security_get_secure_context_from_proxypp(proxypp);
    assert (sc);
    if (!sc->crypto_context->crypto_transform->decode_rtps_message (sc->crypto_context->crypto_transform, &plain_buffer, &encoded_buffer, pm->pp_crypto_handle, pm->proxypp_crypto_handle, &ex))
    {
      if (ex.code == DDS_SECURITY_ERR_INVALID_CRYPTO_RECEIVER_SIGN_CODE)
        continue; /* Could be caused by 'with_origin_authentication' being used, so try next match */
      GVWARNING ("decoding rtps message from remote participant "PGUIDFMT" failed: %s\n", PGUID (proxypp->e.guid), ex.message ? ex.message : "Unknown error");
      DDS_Security_Exception_reset (&ex);
      ddsrt_mutex_unlock (&proxypp->sec_attr->lock);
      return false;
    }
    *dst_buf = plain_buffer._buffer;
    *dst_len = plain_buffer._length;
  }
  ddsrt_mutex_unlock (&proxypp->sec_attr->lock);
  if (*dst_buf == NULL)
  {
    GVTRACE ("No match found for remote participant "PGUIDFMT" for decoding rtps message\n", PGUID (proxypp->e.guid));
    return false;
  }

  return true;
}

bool q_omg_reader_is_submessage_protected(const struct reader *rd)
{
  assert (rd != NULL);
  return rd->sec_attr != NULL && rd->sec_attr->attr.is_submessage_protected;
}

bool encode_payload (struct writer *wr, ddsrt_iovec_t *vec, unsigned char **buf)
{
  *buf = NULL;
  if (!q_omg_writer_is_payload_protected (wr))
    return true;

  unsigned char *enc_buf;
  size_t enc_len;
  if (!q_omg_security_encode_serialized_payload (wr, vec->iov_base, vec->iov_len, &enc_buf, &enc_len))
    return false;

  /* Replace the iov buffer, which should always be aliased. */
  vec->iov_base = (char *) enc_buf;
  vec->iov_len = (ddsrt_iov_len_t) enc_len;
  assert ((size_t) vec->iov_len == enc_len);
  *buf = enc_buf;
  return true;
}


static bool decode_payload (const struct ddsi_domaingv *gv, struct nn_rsample_info *sampleinfo, unsigned char *payloadp, uint32_t *payloadsz, size_t *submsg_len)
{
  assert (payloadp);
  assert (payloadsz);
  assert (*payloadsz);
  assert (submsg_len);
  assert (sampleinfo);

  if (sampleinfo->pwr == NULL)
    /* No specified proxy writer means no encoding. */
    return true;

  /* Only decode when the attributes tell us so. */
  if ((sampleinfo->pwr->c.security_info.security_attributes & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_PAYLOAD_PROTECTED)
      != NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_PAYLOAD_PROTECTED)
    return true;

  unsigned char *dst_buf = NULL;
  size_t dst_len = 0;
  if (!q_omg_security_decode_serialized_payload (sampleinfo->pwr, payloadp, *payloadsz, &dst_buf, &dst_len))
  {
    GVWARNING ("decode_payload: failed to decrypt data from "PGUIDFMT"", PGUID (sampleinfo->pwr->e.guid));
    return false;
  }

  /* Expect result to always fit into the original buffer. */
  assert (*payloadsz >= dst_len);

  /* Reduce submessage and payload lengths. */
  *submsg_len -= *payloadsz - (uint32_t) dst_len;
  *payloadsz = (uint32_t) dst_len;
  memcpy (payloadp, dst_buf, dst_len);
  ddsrt_free (dst_buf);
  return true;
}

bool decode_Data (const struct ddsi_domaingv *gv, struct nn_rsample_info *sampleinfo, unsigned char *payloadp, uint32_t payloadsz, size_t *submsg_len)
{
  /* Only decode when there's actual data. */
  if (payloadp == NULL || payloadsz == 0)
    return true;
  else if (!decode_payload (gv, sampleinfo, payloadp, &payloadsz, submsg_len))
    return false;
  else
  {
    /* It's possible that the payload size (and thus the sample size) has been reduced. */
    sampleinfo->size = payloadsz;
    return true;
  }
}

bool decode_DataFrag (const struct ddsi_domaingv *gv, struct nn_rsample_info *sampleinfo, unsigned char *payloadp, uint32_t payloadsz, size_t *submsg_len)
{
  /* Only decode when there's actual data; do not touch the sampleinfo->size in
     contradiction to decode_Data() (it has been calculated differently). */
  if (payloadp == NULL || payloadsz == 0)
    return true;
  else
    return decode_payload (gv, sampleinfo, payloadp, &payloadsz, submsg_len);
}

void encode_datareader_submsg (struct nn_xmsg *msg, struct nn_xmsg_marker sm_marker, struct proxy_writer *pwr, const struct ddsi_guid *rd_guid)
{
  /* FIXME: avoid this lookup */
  struct reader * const rd = entidx_lookup_reader_guid (pwr->e.gv->entity_index, rd_guid);
  /* surely a reader can only be protected if the participant has security enabled? */
  if (rd == NULL || !q_omg_reader_is_submessage_protected (rd))
    return;
  assert (q_omg_participant_is_secure (rd->c.pp));

  unsigned char *src_buf;
  size_t src_len;
  unsigned char *dst_buf;
  size_t dst_len;

  /* Make one blob of the current sub-message by appending the serialized payload. */
  nn_xmsg_submsg_append_refd_payload (msg, sm_marker);

  /* Get the sub-message buffer. */
  src_buf = nn_xmsg_submsg_from_marker (msg, sm_marker);
  src_len = nn_xmsg_submsg_size (msg, sm_marker);

  if (q_omg_security_encode_datareader_submessage (rd, &pwr->e.guid.prefix, src_buf, src_len, &dst_buf, &dst_len))
  {
    nn_xmsg_submsg_replace (msg, sm_marker, dst_buf, dst_len);
    ddsrt_free (dst_buf);
  }
  else
  {
    /* The sub-message should have been encoded, which failed. Remove it to prevent it from being send. */
    nn_xmsg_submsg_remove (msg, sm_marker);
  }
}

void encode_datawriter_submsg (struct nn_xmsg *msg, struct nn_xmsg_marker sm_marker, struct writer *wr)
{
  if (!q_omg_writer_is_submessage_protected (wr))
    return;

  /* Only encode when needed.  Surely a writer can only be protected if the participant has security enabled? */
  assert (q_omg_participant_is_secure (wr->c.pp));

  unsigned char *src_buf;
  size_t src_len;
  unsigned char *dst_buf;
  size_t dst_len;
  ddsi_guid_prefix_t dst_guid_prefix;
  ddsi_guid_prefix_t *dst = NULL;

  /* Make one blob of the current sub-message by appending the serialized payload. */
  nn_xmsg_submsg_append_refd_payload (msg, sm_marker);

  /* Get the sub-message buffer. */
  src_buf = nn_xmsg_submsg_from_marker (msg, sm_marker);
  src_len = nn_xmsg_submsg_size (msg, sm_marker);

  if (nn_xmsg_getdst1prefix (msg, &dst_guid_prefix))
    dst = &dst_guid_prefix;

  if (q_omg_security_encode_datawriter_submessage (wr, dst, src_buf, src_len, &dst_buf, &dst_len))
  {
    nn_xmsg_submsg_replace (msg, sm_marker, dst_buf, dst_len);
    ddsrt_free (dst_buf);
  }
  else
  {
    /* The sub-message should have been encoded, which failed. Remove it to prevent it from being send. */
    nn_xmsg_submsg_remove (msg, sm_marker);
  }
}

bool validate_msg_decoding (const struct entity_common *e, const struct proxy_endpoint_common *c, const struct proxy_participant *proxypp, const struct receiver_state *rst, SubmessageKind_t prev_smid)
{
  assert (e);
  assert (c);
  assert (proxypp);
  assert (rst);

  /* If this endpoint is expected to have submessages protected, it means that the
   * previous submessage id (prev_smid) has to be SMID_SEC_PREFIX. That caused the
   * protected submessage to be copied into the current RTPS message as a clear
   * submessage, which we are currently handling.
   * However, we have to check if the prev_smid is actually SMID_SEC_PREFIX, otherwise
   * a rascal can inject data as just a clear submessage. */
  if ((c->security_info.security_attributes & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED)
      == NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_SUBMESSAGE_PROTECTED)
  {
    if (prev_smid != SMID_SEC_PREFIX)
      return false;
  }

  /* At this point, we should also check if the complete RTPS message was encoded when
   * that is expected. */
  if (q_omg_security_is_remote_rtps_protected (proxypp, e->guid.entityid) && !rst->rtps_encoded)
  {
    return false;
  }

  return true;
}

static int32_t validate_submsg (struct ddsi_domaingv *gv, SubmessageKind_t smid, const unsigned char *submsg, unsigned char const * const end, int byteswap)
{
  assert (end >= submsg);
  if ((size_t) (end - submsg) < RTPS_SUBMESSAGE_HEADER_SIZE)
  {
    GVWARNING ("Submsg 0x%02x does not fit message", smid);
    return -1;
  }

  SubmessageHeader_t const * const hdr = (SubmessageHeader_t *) submsg;
  if (hdr->submessageId != smid && smid != SMID_PAD)
  {
    GVWARNING("Unexpected submsg 0x%02x (0x%02x expected)", hdr->submessageId, smid);
    return -1;
  }

  uint16_t size = hdr->octetsToNextHeader;
  if (byteswap)
    size = ddsrt_bswap2u (size);
  const int32_t result = (int32_t) size + (int32_t) RTPS_SUBMESSAGE_HEADER_SIZE;
  if (end - submsg < result)
  {
    GVWARNING ("Submsg 0x%02x does not fit message", smid);
    return -1;
  }
  return result;
}

static int32_t padding_submsg (struct ddsi_domaingv *gv, unsigned char *start, unsigned char *end, int byteswap)
{
  assert (end >= start);
  const size_t size = (size_t) (end - start);
  if (size < RTPS_SUBMESSAGE_HEADER_SIZE)
  {
    GVWARNING("Padding submessage doesn't fit");
    return -1;
  }

  assert (size <= UINT16_MAX + RTPS_SUBMESSAGE_HEADER_SIZE);
  SubmessageHeader_t * const padding = (SubmessageHeader_t *) start;
  padding->submessageId = SMID_PAD;
  DDSRT_STATIC_ASSERT (SMFLAG_ENDIANNESS == 1);
  padding->flags = (byteswap ? !(DDSRT_ENDIAN == DDSRT_LITTLE_ENDIAN) : (DDSRT_ENDIAN == DDSRT_LITTLE_ENDIAN));
  padding->octetsToNextHeader = (uint16_t) (size - RTPS_SUBMESSAGE_HEADER_SIZE);
  if (byteswap)
    padding->octetsToNextHeader = ddsrt_bswap2u (padding->octetsToNextHeader);
  return (int32_t) size;
}

static bool decode_SecPrefix_patched_hdr_flags (const struct receiver_state *rst, unsigned char *submsg, size_t submsg_size, unsigned char * const msg_end, const ddsi_guid_prefix_t * const src_prefix, const ddsi_guid_prefix_t * const dst_prefix, int byteswap)
{
  int smsize = -1;
  size_t totalsize = submsg_size;
  unsigned char *body_submsg;
  unsigned char *prefix_submsg;
  unsigned char *postfix_submsg;

  /* First sub-message is the SEC_PREFIX. */
  prefix_submsg = submsg;

  /* Next sub-message is SEC_BODY when encrypted or the original submessage when only signed. */
  body_submsg = submsg + submsg_size;
  if ((smsize = validate_submsg (rst->gv, SMID_PAD, body_submsg, msg_end, byteswap)) <= 0)
    return false;
  totalsize += (size_t) smsize;

  /* Third sub-message should be the SEC_POSTFIX. */
  postfix_submsg = submsg + totalsize;
  if ((smsize = validate_submsg (rst->gv, SMID_SEC_POSTFIX, postfix_submsg, msg_end, byteswap)) <= 0)
    return false;
  totalsize += (size_t) smsize;

  /* Decode all three submessages. */
  unsigned char *dst_buf;
  size_t dst_len;
  const bool decoded = q_omg_security_decode_submessage (rst->gv, src_prefix, dst_prefix, submsg, totalsize, &dst_buf, &dst_len);
  if (decoded && dst_buf)
  {
    /*
     * The 'normal' submessage sequence handling will continue after the
     * given security SEC_PREFIX.
     */
    SubmessageHeader_t const * const body_submsg_hdr = (SubmessageHeader_t const *) body_submsg;
    if (body_submsg_hdr->submessageId == SMID_SEC_BODY)
    {
      /*
       * Copy the decoded buffer into the original message, replacing (part
       * of) SEC_BODY.
       *
       * By replacing the SEC_BODY with the decoded submessage, everything
       * can continue as if there was never an encoded submessage.
       */
      assert (totalsize >= submsg_size);
      assert (dst_len <= totalsize - submsg_size);
      memcpy (body_submsg, dst_buf, dst_len);

      /* Remainder of SEC_BODY & SEC_POSTFIX should be padded to keep the submsg sequence going. */
      smsize = padding_submsg (rst->gv, body_submsg + dst_len, prefix_submsg + totalsize, byteswap);
    }
    else
    {
      /*
       * When only signed, then the submessage is already available and
       * SMID_SEC_POSTFIX will be ignored.
       * So, we don't really have to do anything.
       */
    }
    ddsrt_free (dst_buf);
  }
  else
  {
    /*
     * Decoding or signing failed.
     *
     * Replace the security submessages with padding. This also removes a plain
     * submessage when a signature check failed.
     */
    smsize = padding_submsg (rst->gv, body_submsg, prefix_submsg + totalsize, byteswap);
  }

  return (smsize > 0);
}

bool decode_SecPrefix (const struct receiver_state *rst, unsigned char *submsg, size_t submsg_size, unsigned char * const msg_end, const ddsi_guid_prefix_t * const src_prefix, const ddsi_guid_prefix_t * const dst_prefix, int byteswap)
{
  /* FIXME: eliminate the patching of hdr->flags if possible */
  SubmessageHeader_t *hdr = (SubmessageHeader_t *) submsg;
  const uint8_t saved_flags = hdr->flags;
  if (byteswap)
  {
    if (DDSRT_ENDIAN == DDSRT_LITTLE_ENDIAN)
      hdr->flags |= 0x01;
    else
      hdr->flags &= 0xFE;
  }
  bool result = decode_SecPrefix_patched_hdr_flags (rst, submsg, submsg_size, msg_end, src_prefix, dst_prefix, byteswap);
  hdr->flags = saved_flags;
  return result;
}

static nn_rtps_msg_state_t check_rtps_message_is_secure (struct ddsi_domaingv *gv, Header_t *hdr, const unsigned char *buff, bool isstream, struct proxy_participant **proxypp)
{
  const uint32_t offset = RTPS_MESSAGE_HEADER_SIZE + (isstream ? sizeof (MsgLen_t) : 0);
  const SubmessageHeader_t *submsg = (const SubmessageHeader_t *) (buff + offset);
  if (submsg->submessageId != SMID_SRTPS_PREFIX)
    return NN_RTPS_MSG_STATE_PLAIN;

  ddsi_guid_t guid;
  guid.prefix = hdr->guid_prefix;
  guid.entityid.u = NN_ENTITYID_PARTICIPANT;

  GVTRACE (" from "PGUIDFMT, PGUID (guid));

  if ((*proxypp = entidx_lookup_proxy_participant_guid (gv->entity_index, &guid)) == NULL)
  {
    GVTRACE ("received encoded rtps message from unknown participant\n");
    return NN_RTPS_MSG_STATE_ERROR;
  }
  else if (!proxypp_is_authenticated (*proxypp))
  {
    GVTRACE ("received encoded rtps message from unauthenticated participant\n");
    return NN_RTPS_MSG_STATE_ERROR;
  }
  else
  {
    return NN_RTPS_MSG_STATE_ENCODED;
  }
}

static nn_rtps_msg_state_t
decode_rtps_message_awake (
  struct nn_rmsg **rmsg,
  Header_t **hdr,
  unsigned char **buff,
  ssize_t *sz,
  struct nn_rbufpool *rbpool,
  bool isstream,
  struct proxy_participant *proxypp)
{
  unsigned char *dstbuf;
  unsigned char *srcbuf;
  size_t srclen, dstlen;

  /* Currently the decode_rtps_message returns a new allocated buffer.
   * This could be optimized by providing a pre-allocated nn_rmsg buffer to
   * copy the decoded rtps message in.
   */
  if (isstream)
  {
    /* Remove MsgLen Submessage which was only needed for a stream to determine the end of the message */
    assert (*sz > (ssize_t) sizeof (MsgLen_t));
    srcbuf = *buff + sizeof (MsgLen_t);
    srclen = (size_t) *sz - sizeof (MsgLen_t);
    memmove (srcbuf, *buff, RTPS_MESSAGE_HEADER_SIZE);
  }
  else
  {
    assert (*sz > 0);
    srcbuf = *buff;
    srclen = (size_t) *sz;
  }

  if (!q_omg_security_decode_rtps_message (proxypp, srcbuf, srclen, &dstbuf, &dstlen))
    return NN_RTPS_MSG_STATE_ERROR;
  else
  {
    assert (dstlen <= UINT32_MAX);

    nn_rmsg_commit (*rmsg);
    *rmsg = nn_rmsg_new (rbpool);
    *buff = NN_RMSG_PAYLOAD (*rmsg);

    memcpy(*buff, dstbuf, dstlen);
    nn_rmsg_setsize (*rmsg, (uint32_t) dstlen);

    ddsrt_free (dstbuf);

    *hdr = (Header_t *) *buff;
    (*hdr)->guid_prefix = nn_ntoh_guid_prefix ((*hdr)->guid_prefix);
    *sz = (ssize_t) dstlen;
    assert ((size_t) *sz == dstlen);
    return NN_RTPS_MSG_STATE_ENCODED;
  }
}

nn_rtps_msg_state_t
decode_rtps_message (
  struct thread_state1 * const ts1,
  struct ddsi_domaingv *gv,
  struct nn_rmsg **rmsg,
  Header_t **hdr,
  unsigned char **buff,
  ssize_t *sz,
  struct nn_rbufpool *rbpool,
  bool isstream)
{
  struct proxy_participant *proxypp;
  nn_rtps_msg_state_t ret;
  thread_state_awake_fixed_domain (ts1);
  ret = check_rtps_message_is_secure (gv, *hdr, *buff, isstream, &proxypp);
  if (ret == NN_RTPS_MSG_STATE_ENCODED)
    ret = decode_rtps_message_awake (rmsg, hdr, buff, sz, rbpool, isstream, proxypp);
  thread_state_asleep (ts1);
  return ret;
}

ssize_t
secure_conn_write(
    const struct ddsi_domaingv *gv,
    ddsi_tran_conn_t conn,
    const nn_locator_t *dst,
    size_t niov,
    const ddsrt_iovec_t *iov,
    uint32_t flags,
    MsgLen_t *msg_len,
    bool dst_one,
    nn_msg_sec_info_t *sec_info,
    ddsi_tran_write_fn_t conn_write_cb)
{
  Header_t *hdr;
  ddsi_guid_t guid;
  unsigned char stbuf[2048];
  unsigned char *srcbuf;
  unsigned char *dstbuf;
  size_t srclen, dstlen;
  int64_t dst_handle = 0;

  assert(iov);
  assert(conn);
  assert(msg_len);
  assert(sec_info);
  assert(niov > 0);
  assert(conn_write_cb);

  if (dst_one)
  {
    dst_handle = sec_info->dst_pp_handle;
    if (dst_handle == 0) {
      return -1;
    }
  }

  hdr = (Header_t *) iov[0].iov_base;
  guid.prefix = nn_ntoh_guid_prefix (hdr->guid_prefix);
  guid.entityid.u = NN_ENTITYID_PARTICIPANT;

  /* first determine the size of the message, then select the
   *  on-stack buffer or allocate one on the heap ...
   */
  srclen = 0;
  for (size_t i = 0; i < niov; i++)
  {
    /* Do not copy MsgLen submessage in case of a stream connection */
    if (i != 1 || !conn->m_stream)
      srclen += iov[i].iov_len;
  }
  if (srclen <= sizeof (stbuf))
    srcbuf = stbuf;
  else
    srcbuf = ddsrt_malloc (srclen);

  /* ... then copy data into buffer */
  srclen = 0;
  for (size_t i = 0; i < niov; i++)
  {
    if (i != 1 || !conn->m_stream)
    {
      memcpy (srcbuf + srclen, iov[i].iov_base, iov[i].iov_len);
      srclen += iov[i].iov_len;
    }
  }

  ssize_t ret = -1;
  if (!q_omg_security_encode_rtps_message (gv, sec_info->src_pp_handle, &guid, srcbuf, srclen, &dstbuf, &dstlen, dst_handle))
    ret = -1;
  else
  {
    ddsrt_iovec_t tmp_iov[3];
    size_t tmp_niov;

    if (conn->m_stream)
    {
      /* Add MsgLen submessage after Header */
      assert (dstlen <= UINT32_MAX - sizeof (*msg_len));
      msg_len->length = (uint32_t) (dstlen + sizeof (*msg_len));

      tmp_iov[0].iov_base = dstbuf;
      tmp_iov[0].iov_len = RTPS_MESSAGE_HEADER_SIZE;
      tmp_iov[1].iov_base = (void *) msg_len;
      tmp_iov[1].iov_len = sizeof (*msg_len);
      tmp_iov[2].iov_base = dstbuf + RTPS_MESSAGE_HEADER_SIZE;
      tmp_iov[2].iov_len = (ddsrt_iov_len_t) (dstlen - RTPS_MESSAGE_HEADER_SIZE);
      tmp_niov = 3;
    }
    else
    {
      assert (dstlen <= UINT32_MAX);
      msg_len->length = (uint32_t) dstlen;

      tmp_iov[0].iov_base = dstbuf;
      tmp_iov[0].iov_len = (ddsrt_iov_len_t) dstlen;
      tmp_niov = 1;
    }
    ret = conn_write_cb (conn, dst, tmp_niov, tmp_iov, flags);
    ddsrt_free (dstbuf);
  }

  if (srcbuf != stbuf)
    ddsrt_free (srcbuf);
  return ret;
}

bool q_omg_plist_keyhash_is_protected(const ddsi_plist_t *plist)
{
  assert(plist);
  if (plist->present & PP_ENDPOINT_SECURITY_INFO)
  {
    unsigned attr = plist->endpoint_security_info.security_attributes;
    return attr & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID &&
           attr & NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_KEY_PROTECTED;
  }
  return false;
}

bool q_omg_is_endpoint_protected(const ddsi_plist_t *plist)
{
  assert(plist);
  return plist->present & PP_ENDPOINT_SECURITY_INFO &&
         !SECURITY_INFO_CLEAR(plist->endpoint_security_info, NN_ENDPOINT_SECURITY_ATTRIBUTES_FLAG_IS_VALID);
}

void q_omg_log_endpoint_protection(struct ddsi_domaingv * const gv, const ddsi_plist_t *plist)
{
  GVLOGDISC (" p(");
  if (plist->present & PP_ENDPOINT_SECURITY_INFO)
    GVLOGDISC ("0x%08x.0x%08x", plist->endpoint_security_info.security_attributes, plist->endpoint_security_info.plugin_security_attributes);
  else
    GVLOGDISC ("open");
  GVLOGDISC (")");
}

#else /* DDSI_INCLUDE_SECURITY */

#include "dds/ddsi/ddsi_security_omg.h"

extern inline bool q_omg_security_enabled(void);

extern inline bool q_omg_participant_is_access_protected(UNUSED_ARG(const struct participant *pp));
extern inline bool q_omg_participant_is_rtps_protected(UNUSED_ARG(const struct participant *pp));
extern inline bool q_omg_participant_is_liveliness_protected(UNUSED_ARG(const struct participant *pp));
extern inline bool q_omg_participant_is_secure(UNUSED_ARG(const struct participant *pp));
extern inline bool q_omg_proxy_participant_is_secure(UNUSED_ARG(const struct proxy_participant *proxypp));

extern inline unsigned determine_subscription_writer(UNUSED_ARG(const struct reader *rd));

extern inline bool q_omg_security_match_remote_writer_enabled(UNUSED_ARG(struct reader *rd), UNUSED_ARG(struct proxy_writer *pwr), UNUSED_ARG(int64_t *crypto_handle));
extern inline bool q_omg_security_match_remote_reader_enabled(UNUSED_ARG(struct writer *wr), UNUSED_ARG(struct proxy_reader *prd), UNUSED_ARG(int64_t *crypto_handle));

extern inline bool q_omg_writer_is_discovery_protected(UNUSED_ARG(const struct writer *wr));
extern inline bool q_omg_writer_is_submessage_protected(UNUSED_ARG(const struct writer *wr));
extern inline bool q_omg_writer_is_payload_protected(UNUSED_ARG(const struct writer *wr));

extern inline void q_omg_get_proxy_writer_security_info(UNUSED_ARG(struct proxy_writer *pwr), UNUSED_ARG(const ddsi_plist_t *plist), UNUSED_ARG(nn_security_info_t *info));
extern inline bool q_omg_security_check_remote_writer_permissions(UNUSED_ARG(const struct proxy_writer *pwr), UNUSED_ARG(uint32_t domain_id), UNUSED_ARG(struct participant *pp));
extern inline void q_omg_security_deregister_remote_writer_match(UNUSED_ARG(const struct proxy_writer *pwr), UNUSED_ARG(const struct reader *rd), UNUSED_ARG(struct rd_pwr_match *match));
extern inline void q_omg_get_proxy_reader_security_info(UNUSED_ARG(struct proxy_reader *prd), UNUSED_ARG(const ddsi_plist_t *plist), UNUSED_ARG(nn_security_info_t *info));
extern inline bool q_omg_security_check_remote_reader_permissions(UNUSED_ARG(const struct proxy_reader *prd), UNUSED_ARG(uint32_t domain_id), UNUSED_ARG(struct participant *par));
extern inline void q_omg_security_deregister_remote_reader_match(UNUSED_ARG(const struct proxy_reader *prd), UNUSED_ARG(const struct writer *wr), UNUSED_ARG(struct wr_prd_match *match));

extern inline unsigned determine_publication_writer(UNUSED_ARG(const struct writer *wr));

extern inline bool is_proxy_participant_deletion_allowed(UNUSED_ARG(struct ddsi_domaingv * const gv), UNUSED_ARG(const struct ddsi_guid *guid), UNUSED_ARG(const ddsi_entityid_t pwr_entityid));

extern inline bool q_omg_is_similar_participant_security_info(UNUSED_ARG(struct participant *pp), UNUSED_ARG(struct proxy_participant *proxypp));

extern inline bool q_omg_participant_allow_unauthenticated(UNUSED_ARG(struct participant *pp));

extern inline bool q_omg_security_check_create_participant(UNUSED_ARG(struct participant *pp), UNUSED_ARG(uint32_t domain_id));

extern inline void q_omg_security_deregister_participant(UNUSED_ARG(struct participant *pp));

extern inline bool q_omg_security_check_create_topic(UNUSED_ARG(const struct ddsi_domaingv *gv), UNUSED_ARG(const ddsi_guid_t *pp_guid), UNUSED_ARG(const char *topic_name), UNUSED_ARG(const struct dds_qos *qos));

extern inline int64_t q_omg_security_get_local_participant_handle(UNUSED_ARG(const struct participant *pp));

extern inline bool q_omg_security_check_create_writer(UNUSED_ARG(struct participant *pp), UNUSED_ARG(uint32_t domain_id), UNUSED_ARG(const char *topic_name), UNUSED_ARG(const struct dds_qos *writer_qos));

extern inline void q_omg_security_register_writer(UNUSED_ARG(struct writer *wr));

extern inline void q_omg_security_deregister_writer(UNUSED_ARG(struct writer *wr));

extern inline bool q_omg_security_check_create_reader(UNUSED_ARG(struct participant *pp), UNUSED_ARG(uint32_t domain_id), UNUSED_ARG(const char *topic_name), UNUSED_ARG(const struct dds_qos *reader_qos));

extern inline void q_omg_security_register_reader(UNUSED_ARG(struct reader *rd));

extern inline void q_omg_security_deregister_reader(UNUSED_ARG(struct reader *rd));

extern inline bool q_omg_security_is_remote_rtps_protected(UNUSED_ARG(const struct proxy_participant *proxypp), UNUSED_ARG(ddsi_entityid_t entityid));

/* initialize the proxy participant security attributes */
extern inline void q_omg_security_init_remote_participant(UNUSED_ARG(struct proxy_participant *proxypp));

/* ask to access control security plugin for the remote participant permissions */
extern inline int64_t q_omg_security_check_remote_participant_permissions(UNUSED_ARG(uint32_t domain_id), UNUSED_ARG(struct participant *pp), UNUSED_ARG(struct proxy_participant *proxypp));

extern inline bool q_omg_security_register_remote_participant(UNUSED_ARG(struct participant *pp), UNUSED_ARG(struct proxy_participant *proxypp), UNUSED_ARG(int64_t identity_handle), UNUSED_ARG(int64_t shared_secret));

extern inline void q_omg_security_deregister_remote_participant(UNUSED_ARG(struct proxy_participant *proxypp));

extern inline void q_omg_security_participant_send_tokens(UNUSED_ARG(struct participant *pp), UNUSED_ARG(struct proxy_participant *proxypp));

extern inline void set_proxy_participant_security_info(UNUSED_ARG(struct proxy_participant *prd), UNUSED_ARG(const ddsi_plist_t *plist));

extern inline void set_proxy_reader_security_info(UNUSED_ARG(struct proxy_reader *prd), UNUSED_ARG(const ddsi_plist_t *plist));

extern inline void set_proxy_writer_security_info(UNUSED_ARG(struct proxy_writer *pwr), UNUSED_ARG(const ddsi_plist_t *plist));

extern inline bool decode_Data(
  UNUSED_ARG(const struct ddsi_domaingv *gv),
  UNUSED_ARG(struct nn_rsample_info *sampleinfo),
  UNUSED_ARG(unsigned char *payloadp),
  UNUSED_ARG(uint32_t payloadsz),
  UNUSED_ARG(size_t *submsg_len));

extern inline bool decode_DataFrag(
  UNUSED_ARG(const struct ddsi_domaingv *gv),
  UNUSED_ARG(struct nn_rsample_info *sampleinfo),
  UNUSED_ARG(unsigned char *payloadp),
  UNUSED_ARG(uint32_t payloadsz),
  UNUSED_ARG(size_t *submsg_len));

extern inline void encode_datareader_submsg(
  UNUSED_ARG(struct nn_xmsg *msg),
  UNUSED_ARG(struct nn_xmsg_marker sm_marker),
  UNUSED_ARG(struct proxy_writer *pwr),
  UNUSED_ARG(const struct ddsi_guid *rd_guid));

extern inline void encode_datawriter_submsg(
  UNUSED_ARG(struct nn_xmsg *msg),
  UNUSED_ARG(struct nn_xmsg_marker sm_marker),
  UNUSED_ARG(struct writer *wr));

extern inline bool validate_msg_decoding(
  UNUSED_ARG(const struct entity_common *e),
  UNUSED_ARG(const struct proxy_endpoint_common *c),
  UNUSED_ARG(struct proxy_participant *proxypp),
  UNUSED_ARG(struct receiver_state *rst),
  UNUSED_ARG(SubmessageKind_t prev_smid));

extern inline int decode_SecPrefix(
  UNUSED_ARG(struct receiver_state *rst),
  UNUSED_ARG(unsigned char *submsg),
  UNUSED_ARG(size_t submsg_size),
  UNUSED_ARG(unsigned char * const msg_end),
  UNUSED_ARG(const ddsi_guid_prefix_t * const src_prefix),
  UNUSED_ARG(const ddsi_guid_prefix_t * const dst_prefix),
  UNUSED_ARG(int byteswap));

extern inline nn_rtps_msg_state_t decode_rtps_message(
  UNUSED_ARG(struct thread_state1 * const ts1),
  UNUSED_ARG(struct ddsi_domaingv *gv),
  UNUSED_ARG(struct nn_rmsg **rmsg),
  UNUSED_ARG(Header_t **hdr),
  UNUSED_ARG(unsigned char **buff),
  UNUSED_ARG(ssize_t *sz),
  UNUSED_ARG(struct nn_rbufpool *rbpool),
  UNUSED_ARG(bool isstream));

extern inline int64_t q_omg_security_get_remote_participant_handle(UNUSED_ARG(struct proxy_participant *proxypp));

extern inline bool q_omg_reader_is_discovery_protected(UNUSED_ARG(const struct reader *rd));

extern inline bool q_omg_reader_is_submessage_protected(UNUSED_ARG(const struct reader *rd));

extern inline bool q_omg_plist_keyhash_is_protected(UNUSED_ARG(const ddsi_plist_t *plist));

extern inline bool q_omg_is_endpoint_protected(UNUSED_ARG(const ddsi_plist_t *plist));

extern inline void q_omg_log_endpoint_protection(UNUSED_ARG(struct ddsi_domaingv * const gv), UNUSED_ARG(const ddsi_plist_t *plist));


#endif /* DDSI_INCLUDE_SECURITY */
