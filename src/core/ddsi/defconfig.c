#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "dds/ddsi/ddsi_config.h"

void ddsi_config_init_default (struct ddsi_config *cfg)
{
  memset (cfg, 0, sizeof (*cfg));
  static char *networkRecvAddressStrings_init_[] = {
    "preferred",
    NULL
  };
  cfg->networkRecvAddressStrings = networkRecvAddressStrings_init_;
  cfg->externalMaskString = "0.0.0.0";
  cfg->allowMulticast = UINT32_C (2147483648);
  cfg->multicast_ttl = INT32_C (32);
  cfg->transport_selector = INT32_C (1);
  cfg->enableMulticastLoopback = INT32_C (1);
  cfg->max_msg_size = UINT32_C (14720);
  cfg->max_rexmit_msg_size = UINT32_C (1456);
  cfg->fragment_size = UINT16_C (1344);
#ifdef DDS_HAS_SECURITY
#endif /* DDS_HAS_SECURITY */
#ifdef DDS_HAS_NETWORK_PARTITIONS
#endif /* DDS_HAS_NETWORK_PARTITIONS */
  cfg->rbuf_size = UINT32_C (1048576);
  cfg->rmsg_chunk_size = UINT32_C (131072);
  cfg->standards_conformance = INT32_C (2);
  cfg->many_sockets_mode = INT32_C (1);
  cfg->domainTag = "";
  cfg->extDomainId.isdefault = 1;
  cfg->ds_grace_period = INT64_C (30000000000);
  cfg->participantIndex = INT32_C (-2);
  cfg->maxAutoParticipantIndex = INT32_C (9);
  cfg->spdpMulticastAddressString = "239.255.0.1";
  cfg->spdp_interval = INT64_C (30000000000);
  cfg->ports.base = UINT32_C (7400);
  cfg->ports.dg = UINT32_C (250);
  cfg->ports.pg = UINT32_C (2);
  cfg->ports.d1 = UINT32_C (10);
  cfg->ports.d2 = UINT32_C (1);
  cfg->ports.d3 = UINT32_C (11);
#ifdef DDS_HAS_TOPIC_DISCOVERY
#endif /* DDS_HAS_TOPIC_DISCOVERY */
  cfg->lease_duration = INT64_C (10000000000);
  cfg->tracefile = "cyclonedds.log";
  cfg->pcap_file = "";
  cfg->delivery_queue_maxsamples = UINT32_C (256);
  cfg->primary_reorder_maxsamples = UINT32_C (128);
  cfg->secondary_reorder_maxsamples = UINT32_C (128);
  cfg->defrag_unreliable_maxsamples = UINT32_C (4);
  cfg->defrag_reliable_maxsamples = UINT32_C (16);
  cfg->besmode = INT32_C (1);
  cfg->unicast_response_to_spdp_messages = INT32_C (1);
  cfg->synchronous_delivery_latency_bound = INT64_C (9223372036854775807);
  cfg->retransmit_merging_period = INT64_C (5000000);
  cfg->const_hb_intv_sched = INT64_C (100000000);
  cfg->const_hb_intv_min = INT64_C (5000000);
  cfg->const_hb_intv_sched_min = INT64_C (20000000);
  cfg->const_hb_intv_sched_max = INT64_C (8000000000);
  cfg->max_queued_rexmit_bytes = UINT32_C (524288);
  cfg->max_queued_rexmit_msgs = UINT32_C (200);
  cfg->writer_linger_duration = INT64_C (1000000000);
  cfg->socket_rcvbuf_size.min.isdefault = 1;
  cfg->socket_rcvbuf_size.max.isdefault = 1;
  cfg->socket_sndbuf_size.min.isdefault = 0;
  cfg->socket_sndbuf_size.min.value = UINT32_C (65536);
  cfg->socket_sndbuf_size.max.isdefault = 1;
  cfg->nack_delay = INT64_C (100000000);
  cfg->ack_delay = INT64_C (10000000);
  cfg->auto_resched_nack_delay = INT64_C (3000000000);
  cfg->preemptive_ack_delay = INT64_C (10000000);
  cfg->ddsi2direct_max_threads = UINT32_C (1);
  cfg->max_sample_size = UINT32_C (2147483647);
  cfg->noprogress_log_stacktraces = INT32_C (1);
  cfg->liveliness_monitoring_interval = INT64_C (1000000000);
  cfg->monitor_port = INT32_C (-1);
  cfg->prioritize_retransmit = INT32_C (1);
  cfg->recv_thread_stop_maxretries = UINT32_C (4294967295);
  cfg->whc_lowwater_mark = UINT32_C (1024);
  cfg->whc_highwater_mark = UINT32_C (512000);
  cfg->whc_init_highwater_mark.isdefault = 0;
  cfg->whc_init_highwater_mark.value = UINT32_C (30720);
  cfg->whc_adaptive = INT32_C (1);
  cfg->max_rexmit_burst_size = UINT32_C (1048576);
  cfg->init_transmit_extra_pct = UINT32_C (4294967295);
  cfg->tcp_nodelay = INT32_C (1);
  cfg->tcp_port = INT32_C (-1);
  cfg->tcp_read_timeout = INT64_C (2000000000);
  cfg->tcp_write_timeout = INT64_C (2000000000);
#ifdef DDS_HAS_SSL
  cfg->ssl_verify = INT32_C (1);
  cfg->ssl_verify_client = INT32_C (1);
  cfg->ssl_keystore = "keystore";
  cfg->ssl_key_pass = "secret";
  cfg->ssl_ciphers = "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH";
  cfg->ssl_rand_file = "";
  cfg->ssl_min_version.major = 1;
  cfg->ssl_min_version.minor = 3;
#endif /* DDS_HAS_SSL */
#ifdef DDS_HAS_SHM
  cfg->shm_locator = "";
  cfg->iceoryx_service = "DDS_CYCLONE";
  cfg->shm_log_lvl = INT32_C (4);
#endif /* DDS_HAS_SHM */
}
/* generated from ddsi_config.h[75edea6617af11bacc46f91e519773f6df580655] */
/* generated from ddsi_cfgunits.h[fc550f1620aa20dcd9244ef4e24299d5001efbb4] */
/* generated from ddsi_cfgelems.h[11913cd398f1cd1b52e06d924718df62a5981beb] */
/* generated from ddsi_config.c[ed9898f72f9dbcfa20ce7706835da091efcea0ca] */
/* generated from _confgen.h[f2d235d5551cbf920a8a2962831dddeabd2856ac] */
/* generated from _confgen.c[c23ec2e896226a424a21f1a4b343cb780056f52e] */
/* generated from generate_rnc.c[79379fcd8615d777c20c0c0324547bfdcc180c60] */
/* generated from generate_md.c[f3669b494645cfff74758b587b4a552910901378] */
/* generated from generate_rst.c[1e95348d13f06a441bc0cef8731e4ffeb99cad1a] */
/* generated from generate_xsd.c[22ae895de728df202f681642178300f14102af65] */
/* generated from generate_defconfig.c[e2f6526ef516323caa0b0f497c493f69d02a40ff] */
