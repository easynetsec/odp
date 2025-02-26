/* Copyright (c) 2014-2018, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

/* enable strtok */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>

#include <openssl/des.h>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>

#include <odp_api.h>

#include <odp/helper/odph_api.h>

#include <odp_ipsec_stream.h>

#define STREAM_MAGIC 0xBABE01234567CAFE

#define LOOP_DEQ_COUNT        32    /**< packets to dequeue at once */

/* Ignore warnings about APIs deprecated in OpenSSL 3.0 */
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

/**
 * Stream packet header
 */
typedef struct ODP_PACKED stream_pkt_hdr_s {
	odp_u64be_t magic;   /**< Stream magic value for verification */
	uint8_t    data[];  /**< Incrementing data stream */
} stream_pkt_hdr_t;

stream_db_t *stream_db;

void init_stream_db(void)
{
	odp_shm_t shm;

	shm = odp_shm_reserve("stream_db",
			      sizeof(stream_db_t),
			      ODP_CACHE_LINE_SIZE,
			      0);

	if (shm == ODP_SHM_INVALID) {
		ODPH_ERR("Error: shared mem reserve failed.\n");
		exit(EXIT_FAILURE);
	}

	stream_db = odp_shm_addr(shm);

	if (stream_db == NULL) {
		ODPH_ERR("Error: shared mem alloc failed.\n");
		exit(EXIT_FAILURE);
	}
	memset(stream_db, 0, sizeof(*stream_db));
}

int create_stream_db_entry(char *input)
{
	int pos = 0;
	char *local;
	char *str;
	char *save;
	char *token;
	stream_db_entry_t *entry = &stream_db->array[stream_db->index];

	/* Verify we have a good entry */
	if (MAX_DB <= stream_db->index)
		return -1;

	/* Make a local copy */
	local = malloc(strlen(input) + 1);
	if (NULL == local)
		return -1;
	strcpy(local, input);

	/* Setup for using "strtok_r" to search input string */
	str = local;
	save = NULL;

	/* Parse tokens separated by ',' */
	while (NULL != (token = strtok_r(str, ",", &save))) {
		str = NULL;  /* reset str for subsequent strtok_r calls */

		/* Parse token based on its position */
		switch (pos) {
		case 0:
			parse_ipv4_string(token, &entry->src_ip, NULL);
			break;
		case 1:
			parse_ipv4_string(token, &entry->dst_ip, NULL);
			break;
		case 2:
			entry->input.intf = strdup(token);
			break;
		case 3:
			entry->output.intf = strdup(token);
			break;
		case 4:
			entry->count = atoi(token);
			break;
		case 5:
			entry->length = atoi(token);
			if (entry->length < sizeof(stream_pkt_hdr_t))
				entry->length = 0;
			else
				entry->length -= sizeof(stream_pkt_hdr_t);
			break;
		default:
			printf("ERROR: extra token \"%s\" at position %d\n",
			       token, pos);
			break;
		}

		/* Advance to next position */
		pos++;
	}

	/* Verify we parsed exactly the number of tokens we expected */
	if (6 != pos) {
		printf("ERROR: \"%s\" contains %d tokens, expected 6\n",
		       input,
		       pos);
		free(local);
		return -1;
	}

	/* Add stream to the list */
	entry->id = stream_db->index++;
	entry->next = stream_db->list;
	stream_db->list = entry;

	free(local);
	return 0;
}

static const EVP_MD *get_evp_md(odp_auth_alg_t auth)
{
	const EVP_MD *evp_md;

	switch (auth) {
	case ODP_AUTH_ALG_MD5_HMAC:
		evp_md = EVP_md5();
		break;
	case ODP_AUTH_ALG_SHA1_HMAC:
		evp_md = EVP_sha1();
		break;
	case ODP_AUTH_ALG_SHA256_HMAC:
		evp_md = EVP_sha256();
		break;
	default:
		evp_md = NULL;
	}

	return evp_md;
}

void resolve_stream_db(void)
{
	stream_db_entry_t *stream = NULL;

	/* For each stream look for input and output IPsec entries */
	for (stream = stream_db->list; NULL != stream; stream = stream->next) {
		ipsec_cache_entry_t *entry;

		/* Lookup input entry */
		entry = find_ipsec_cache_entry_in(stream->src_ip,
						  stream->dst_ip,
						  NULL,
						  NULL);
		stream->input.entry = entry;

		stream->input.pktio = odp_pktio_lookup(stream->input.intf);

		if (entry)
			stream->evp_md = get_evp_md(entry->ah.alg);

		/* Lookup output entry */
		entry = find_ipsec_cache_entry_out(stream->src_ip,
						   stream->dst_ip,
						   0);
		stream->output.entry = entry;

		stream->output.pktio = odp_pktio_lookup(stream->output.intf);

		if (stream->evp_md == NULL && entry)
			stream->evp_md = get_evp_md(entry->ah.alg);
	}
}

odp_packet_t create_ipv4_packet(stream_db_entry_t *stream,
				uint8_t *dmac,
				odp_pool_t pkt_pool,
				uint32_t max_len)
{
	ipsec_cache_entry_t *entry = NULL;
	odp_packet_t pkt;
	uint8_t *base;
	uint8_t *data;
	odph_ethhdr_t *eth;
	odph_ipv4hdr_t *ip;
	odph_ipv4hdr_t *inner_ip = NULL;
	odph_ahhdr_t *ah = NULL;
	odph_esphdr_t *esp = NULL;
	odph_icmphdr_t *icmp;
	stream_pkt_hdr_t *test;
	unsigned i;

	if (stream->input.entry)
		entry = stream->input.entry;
	else if (stream->output.entry)
		entry = stream->output.entry;

	/* Make sure there is enough space for protocol overhead */
	if ((stream->length + 200) > max_len) {
		ODPH_ERR("Error: too large test packet\n");
		return ODP_PACKET_INVALID;
	}

	/* Get packet */
	pkt = odp_packet_alloc(pkt_pool, max_len);
	if (ODP_PACKET_INVALID == pkt) {
		ODPH_ERR("Error: packet alloc failed\n");
		return ODP_PACKET_INVALID;
	}
	base = odp_packet_data(pkt);
	data = odp_packet_data(pkt);

	/* Ethernet */
	odp_packet_has_eth_set(pkt, 1);
	eth = (odph_ethhdr_t *)data;
	data += sizeof(*eth);

	memset((char *)eth->src.addr, (0x80 | stream->id), ODPH_ETHADDR_LEN);
	memcpy((char *)eth->dst.addr, dmac, ODPH_ETHADDR_LEN);
	eth->type = odp_cpu_to_be_16(ODPH_ETHTYPE_IPV4);

	/* IPv4 */
	odp_packet_has_ipv4_set(pkt, 1);
	ip = (odph_ipv4hdr_t *)data;
	data += sizeof(*ip);

	/* Wait until almost finished to fill in mutable fields */
	memset((char *)ip, 0, sizeof(*ip));
	ip->ver_ihl = 0x45;
	ip->id = odp_cpu_to_be_16(stream->id);
	/* Outer IP header in tunnel mode */
	if (entry && entry->mode == IPSEC_SA_MODE_TUNNEL &&
	    (entry == stream->input.entry)) {
		ip->proto = ODPH_IPV4;
		ip->src_addr = odp_cpu_to_be_32(entry->tun_src_ip);
		ip->dst_addr = odp_cpu_to_be_32(entry->tun_dst_ip);
	} else {
		ip->proto = ODPH_IPPROTO_ICMPV4;
		ip->src_addr = odp_cpu_to_be_32(stream->src_ip);
		ip->dst_addr = odp_cpu_to_be_32(stream->dst_ip);
	}

	/* AH (if specified) */
	if (entry && (entry == stream->input.entry) &&
	    (ODP_AUTH_ALG_NULL != entry->ah.alg)) {
		if (entry->ah.alg != ODP_AUTH_ALG_MD5_HMAC &&
		    entry->ah.alg != ODP_AUTH_ALG_SHA1_HMAC &&
		    entry->ah.alg != ODP_AUTH_ALG_SHA256_HMAC)
			abort();

		ah = (odph_ahhdr_t *)data;
		data += sizeof(*ah);
		data += entry->ah.icv_len;

		memset((char *)ah, 0, sizeof(*ah) + entry->ah.icv_len);
		ah->ah_len = 1 + (entry->ah.icv_len / 4);
		ah->spi = odp_cpu_to_be_32(entry->ah.spi);
		ah->seq_no = odp_cpu_to_be_32(stream->input.ah_seq++);
	}

	/* ESP (if specified) */
	if (entry && (entry == stream->input.entry) &&
	    (ODP_CIPHER_ALG_NULL != entry->esp.alg)) {
		if (ODP_CIPHER_ALG_3DES_CBC != entry->esp.alg)
			abort();

		esp = (odph_esphdr_t *)data;
		data += sizeof(*esp);
		data += entry->esp.iv_len;

		esp->spi = odp_cpu_to_be_32(entry->esp.spi);
		esp->seq_no = odp_cpu_to_be_32(stream->input.esp_seq++);
		RAND_bytes(esp->iv, 8);
	}

	/* Inner IP header in tunnel mode */
	if (entry && (entry == stream->input.entry) &&
	    (entry->mode == IPSEC_SA_MODE_TUNNEL)) {
		inner_ip = (odph_ipv4hdr_t *)data;
		memset((char *)inner_ip, 0, sizeof(*inner_ip));
		inner_ip->ver_ihl = 0x45;
		inner_ip->tot_len = odp_cpu_to_be_16(sizeof(odph_ipv4hdr_t) +
						     sizeof(odph_icmphdr_t) +
						     sizeof(stream_pkt_hdr_t) +
						     stream->length);
		inner_ip->proto = ODPH_IPPROTO_ICMPV4;
		inner_ip->id = odp_cpu_to_be_16(stream->id);
		inner_ip->ttl = 64;
		inner_ip->tos = 0;
		inner_ip->frag_offset = 0;
		inner_ip->src_addr = odp_cpu_to_be_32(stream->src_ip);
		inner_ip->dst_addr = odp_cpu_to_be_32(stream->dst_ip);
		inner_ip->chksum = ~odp_chksum_ones_comp16(inner_ip,
							   sizeof(*inner_ip));
		data += sizeof(*inner_ip);
	}

	/* ICMP header so we can see it on wireshark */
	icmp = (odph_icmphdr_t *)data;
	data += sizeof(*icmp);
	icmp->type = ICMP_ECHO;
	icmp->code = 0;
	icmp->un.echo.id = odp_cpu_to_be_16(0x1234);
	icmp->un.echo.sequence = odp_cpu_to_be_16(stream->created);

	/* Packet payload of incrementing bytes */
	test = (stream_pkt_hdr_t *)data;
	data += sizeof(*test);
	test->magic = odp_cpu_to_be_64(STREAM_MAGIC);
	for (i = 0; i < stream->length; i++)
		*data++ = (uint8_t)i;

	/* Close ICMP */
	icmp->chksum = 0;
	icmp->chksum = ~odp_chksum_ones_comp16(icmp, data - (uint8_t *)icmp);

	/* Close ESP if specified */
	if (esp) {
		int payload_len = data - (uint8_t *)icmp;
		uint8_t *encrypt_start = (uint8_t *)icmp;

		if (entry->mode == IPSEC_SA_MODE_TUNNEL) {
			payload_len = data - (uint8_t *)inner_ip;
			encrypt_start = (uint8_t *)inner_ip;
		}

		int encrypt_len;
		odph_esptrl_t *esp_t;
		DES_key_schedule ks1, ks2, ks3;
		uint8_t iv[8];

		memcpy(iv, esp->iv, sizeof(iv));

		encrypt_len = ESP_ENCODE_LEN(payload_len + sizeof(*esp_t),
					     entry->esp.block_len);
		for (int n = 0; n < encrypt_len - payload_len; n++)
			*data++ = n + 1;

		esp_t = (odph_esptrl_t *)(data) - 1;
		esp_t->pad_len = encrypt_len - payload_len - sizeof(*esp_t);
		esp_t->next_header = ip->proto;
		ip->proto = ODPH_IPPROTO_ESP;

		DES_set_key((DES_cblock *)&entry->esp.key.data[0], &ks1);
		DES_set_key((DES_cblock *)&entry->esp.key.data[8], &ks2);
		DES_set_key((DES_cblock *)&entry->esp.key.data[16], &ks3);

		DES_ede3_cbc_encrypt(encrypt_start,
				     encrypt_start,
				     encrypt_len,
				     &ks1,
				     &ks2,
				     &ks3,
				     (DES_cblock *)iv,
				     1);
	}

	/* Since ESP can pad we can now fix IP length */
	ip->tot_len = odp_cpu_to_be_16(data - (uint8_t *)ip);

	/* Close AH if specified */
	if (ah) {
		uint8_t hash[EVP_MAX_MD_SIZE];
		int auth_len = data - (uint8_t *)ip;

		ah->next_header = ip->proto;
		ip->proto = ODPH_IPPROTO_AH;

		HMAC(stream->evp_md,
		     entry->ah.key.data,
		     entry->ah.key.length,
		     (uint8_t *)ip,
		     auth_len,
		     hash,
		     NULL);

		memcpy(ah->icv, hash, entry->ah.icv_len);
	}

	/* Correct set packet length offsets */
	odp_packet_pull_tail(pkt, max_len - (data - base));
	odp_packet_l2_offset_set(pkt, (uint8_t *)eth - base);
	odp_packet_l3_offset_set(pkt, (uint8_t *)ip - base);
	odp_packet_l4_offset_set(pkt, ((uint8_t *)ip - base) + sizeof(*ip));

	/* Now fill in final IP header fields */
	ip->ttl = 64;
	ip->tos = 0;
	ip->frag_offset = 0;
	ip->chksum = 0;
	odph_ipv4_csum_update(pkt);
	return pkt;
}

odp_bool_t verify_ipv4_packet(stream_db_entry_t *stream,
			      odp_packet_t pkt)
{
	ipsec_cache_entry_t *entry = NULL;
	uint8_t *data;
	odph_ipv4hdr_t *ip;
	odph_ahhdr_t *ah = NULL;
	odph_esphdr_t *esp = NULL;
	int hdr_len;
	odph_icmphdr_t *icmp;
	stream_pkt_hdr_t *test;
	uint32_t src_ip, dst_ip;

	if (stream->input.entry)
		entry = stream->input.entry;
	else if (stream->output.entry)
		entry = stream->output.entry;

	/* Basic IPv4 verify (add checksum verification) */
	data = odp_packet_l3_ptr(pkt, NULL);
	ip = (odph_ipv4hdr_t *)data;
	data += sizeof(*ip);
	if (0x45 != ip->ver_ihl)
		return FALSE;

	src_ip = odp_be_to_cpu_32(ip->src_addr);
	dst_ip = odp_be_to_cpu_32(ip->dst_addr);
	if ((stream->src_ip != src_ip) && stream->output.entry &&
	    (stream->output.entry->tun_src_ip != src_ip))
		return FALSE;
	if ((stream->dst_ip != dst_ip) && stream->output.entry &&
	    (stream->output.entry->tun_dst_ip != dst_ip))
		return FALSE;

	if ((stream->src_ip != src_ip) && stream->input.entry &&
	    (stream->input.entry->tun_src_ip != src_ip))
		return FALSE;
	if ((stream->dst_ip != dst_ip) && stream->input.entry &&
	    (stream->input.entry->tun_dst_ip != dst_ip))
		return FALSE;

	/* Find IPsec headers if any and compare against entry */
	hdr_len = locate_ipsec_headers(ip, &ah, &esp);

	/* Verify if the packet is IPsec encapsulated or is cleartext as
	 * expected
	 */
	if (((stream->output.entry && (!ah && !esp))) ||
	    (stream->input.entry && (ah || esp)))
		return FALSE;

	/* Cleartext packet */
	if (!ah && !esp)
		goto clear_packet;
	if (ah) {
		if (!entry)
			return FALSE;
		if (ODP_AUTH_ALG_NULL == entry->ah.alg)
			return FALSE;
		if (odp_be_to_cpu_32(ah->spi) != entry->ah.spi)
			return FALSE;
		if (ODP_AUTH_ALG_MD5_HMAC != entry->ah.alg &&
		    ODP_AUTH_ALG_SHA1_HMAC != entry->ah.alg &&
		    ODP_AUTH_ALG_SHA256_HMAC != entry->ah.alg)
			abort();
	} else {
		if (entry && (ODP_AUTH_ALG_NULL != entry->ah.alg))
			return FALSE;
	}
	if (esp) {
		if (!entry)
			return FALSE;
		if (ODP_CIPHER_ALG_NULL == entry->esp.alg)
			return FALSE;
		if (odp_be_to_cpu_32(esp->spi) != entry->esp.spi)
			return FALSE;
		if (ODP_CIPHER_ALG_3DES_CBC != entry->esp.alg)
			abort();
		hdr_len += entry->esp.iv_len;
	} else {
		if (entry && (ODP_CIPHER_ALG_NULL != entry->esp.alg))
			return FALSE;
	}
	data += hdr_len;

	/* Verify authentication (if present) */
	if (ah) {
		uint8_t  ip_tos;
		uint8_t  ip_ttl;
		uint16_t ip_frag_offset;
		uint8_t  icv[entry->ah.icv_len];
		uint8_t  hash[EVP_MAX_MD_SIZE];

		/* Save/clear mutable fields */
		ip_tos = ip->tos;
		ip_ttl = ip->ttl;
		ip_frag_offset = odp_be_to_cpu_16(ip->frag_offset);
		ip->tos = 0;
		ip->ttl = 0;
		ip->frag_offset = 0;
		ip->chksum = 0;
		memcpy(icv, ah->icv, entry->ah.icv_len);
		memset(ah->icv, 0, entry->ah.icv_len);

		/* Calculate HMAC and compare */
		HMAC(stream->evp_md,
		     entry->ah.key.data,
		     entry->ah.key.length,
		     (uint8_t *)ip,
		     odp_be_to_cpu_16(ip->tot_len),
		     hash,
		     NULL);

		if (0 != memcmp(icv, hash, sizeof(icv)))
			return FALSE;

		ip->proto = ah->next_header;
		ip->tos = ip_tos;
		ip->ttl = ip_ttl;
		ip->frag_offset = odp_cpu_to_be_16(ip_frag_offset);
	}

	/* Decipher if present */
	if (esp) {
		odph_esptrl_t *esp_t;
		DES_key_schedule ks1, ks2, ks3;
		uint8_t iv[8];
		int encrypt_len = ipv4_data_len(ip) - hdr_len;

		memcpy(iv, esp->iv, sizeof(iv));

		DES_set_key((DES_cblock *)&entry->esp.key.data[0], &ks1);
		DES_set_key((DES_cblock *)&entry->esp.key.data[8], &ks2);
		DES_set_key((DES_cblock *)&entry->esp.key.data[16], &ks3);

		DES_ede3_cbc_encrypt((uint8_t *)data,
				     (uint8_t *)data,
				     encrypt_len,
				     &ks1,
				     &ks2,
				     &ks3,
				     (DES_cblock *)iv,
				     0);

		esp_t = (odph_esptrl_t *)(data + encrypt_len) - 1;
		ip->proto = esp_t->next_header;
	}

clear_packet:
	/* Verify IP/ICMP packet */
	if (entry && (entry->mode == IPSEC_SA_MODE_TUNNEL) && (ah || esp)) {
		if (ODPH_IPV4 != ip->proto)
			return FALSE;
		odph_ipv4hdr_t *inner_ip = (odph_ipv4hdr_t *)data;

		icmp = (odph_icmphdr_t *)(inner_ip + 1);
		data = (uint8_t *)icmp;
	} else {
		if (ODPH_IPPROTO_ICMPV4 != ip->proto)
			return FALSE;
		icmp = (odph_icmphdr_t *)data;
	}

	/* Verify ICMP header */
	data += sizeof(*icmp);
	if (ICMP_ECHO != icmp->type)
		return FALSE;
	if (0x1234 != odp_be_to_cpu_16(icmp->un.echo.id))
		return FALSE;

	/* Now check our packet */
	test = (stream_pkt_hdr_t *)data;
	if (STREAM_MAGIC != odp_be_to_cpu_64(test->magic))
		return FALSE;

	return TRUE;
}

int create_stream_db_inputs(void)
{
	int created = 0;
	odp_pool_t pkt_pool;
	odp_pool_info_t pool_info;
	stream_db_entry_t *stream = NULL;
	uint32_t max_len;

	/* Lookup the packet pool */
	pkt_pool = odp_pool_lookup("packet_pool");
	if (pkt_pool == ODP_POOL_INVALID) {
		ODPH_ERR("Error: pkt_pool not found\n");
		return -1;
	}
	if (odp_pool_info(pkt_pool, &pool_info)) {
		ODPH_ERR("Error: pool info failed\n");
		return -1;
	}

	/* Only single segment packets are supported */
	max_len = pool_info.params.pkt.seg_len;

	/* For each stream create corresponding input packets */
	for (stream = stream_db->list; NULL != stream; stream = stream->next) {
		int count;
		int ret;
		uint8_t dmac[ODPH_ETHADDR_LEN];
		odp_pktout_queue_t queue;

		ret = odp_pktio_mac_addr(stream->input.pktio,
					 dmac, sizeof(dmac));
		if (ret <= 0) {
			ODPH_ERR("Error: failed during MAC address get for "
				 "%s\n", stream->input.intf);
			continue;
		}

		ret = odp_pktout_queue(stream->input.pktio, &queue, 1);
		if (ret < 1) {
			ODPH_ERR("Error: failed to get outqueue for %s\n",
				 stream->input.intf);
			continue;
		}

		for (count = stream->count; count > 0; count--) {
			odp_packet_t pkt;

			pkt = create_ipv4_packet(stream, dmac, pkt_pool, max_len);
			if (ODP_PACKET_INVALID == pkt) {
				ODPH_ERR("Error: packet buffers exhausted\n");
				break;
			}
			stream->created++;
			if (odp_pktout_send(queue, &pkt, 1) != 1) {
				odp_packet_free(pkt);
				ODPH_ERR("Error: queue enqueue failed\n");
				break;
			}

			/* Count this stream when we create first packet */
			if (1 == stream->created)
				created++;
		}
	}
	if ((stream_db->index > 0) && created == 0) {
		ODPH_ERR("Error: failed to create any input streams\n");
		return -1;
	}

	return created;
}

odp_bool_t verify_stream_db_outputs(void)
{
	odp_bool_t done = TRUE;
	stream_db_entry_t *stream = NULL;
	const char *env;

	env = getenv("ODP_IPSEC_STREAM_VERIFY_MDEQ");
	/* For each stream look for output packets */
	for (stream = stream_db->list; NULL != stream; stream = stream->next) {
		int idx;
		int count;
		int ret;
		odp_queue_t queue;
		odp_event_t ev_tbl[LOOP_DEQ_COUNT];

		ret = odp_pktin_event_queue(stream->output.pktio, &queue, 1);
		if (ret < 1) {
			ODPH_ERR("Error: failed to get inqueue for %s\n",
				 stream->output.intf);
			continue;
		}

		for (;;) {
			if (env) {
				count = odp_queue_deq_multi(queue,
							    ev_tbl,
							    LOOP_DEQ_COUNT);
			} else {
				ev_tbl[0] = odp_queue_deq(queue);
				count = (ev_tbl[0] != ODP_EVENT_INVALID) ?
					1 : 0;
			}
			if (!count)
				break;
			for (idx = 0; idx < count; idx++) {
				odp_bool_t good;
				odp_packet_t pkt;

				pkt = odp_packet_from_event(ev_tbl[idx]);

				good = verify_ipv4_packet(stream, pkt);
				if (good)
					stream->verified++;
				odp_packet_free(pkt);
			}
		}

		printf("Stream %d %d\n", stream->created, stream->verified);

		if (stream->created != stream->verified)
			done = FALSE;
	}
	return done;
}
