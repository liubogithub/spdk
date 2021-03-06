/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk_internal/event.h"
#include "spdk_internal/bdev.h"
#include "spdk_internal/log.h"

#include "gpt.h"

#include "spdk/event.h"
#include "spdk/endian.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"

#define GPT_PRIMARY_PARTITION_TABLE_LBA 0x1
#define PRIMARY_PARTITION_NUMBER 4
#define GPT_PROTECTIVE_MBR 1
#define SPDK_MAX_NUM_PARTITION_ENTRIES 128
#define SPDK_GPT_CRC32_POLYNOMIAL_REFLECT 0xedb88320UL

static uint32_t spdk_gpt_crc32_table[256];

__attribute__((constructor)) static void
spdk_gpt_init_crc32(void)
{
	int i, j;
	uint32_t val;

	for (i = 0; i < 256; i++) {
		val = i;
		for (j = 0; j < 8; j++) {
			if (val & 1) {
				val = (val >> 1) ^ SPDK_GPT_CRC32_POLYNOMIAL_REFLECT;
			} else {
				val = (val >> 1);
			}
		}
		spdk_gpt_crc32_table[i] = val;
	}
}

static uint32_t
spdk_gpt_crc32(const uint8_t *buf, uint32_t size, uint32_t seed)
{
	uint32_t i, crc32 = seed;

	for (i = 0; i < size; i++) {
		crc32 = spdk_gpt_crc32_table[(crc32 ^ buf[i]) & 0xff] ^ (crc32 >> 8);
	}

	return crc32 ^ seed;
}

static int
spdk_gpt_read_partitions(struct spdk_gpt *gpt)
{
	uint32_t total_partition_size, num_partition_entries, partition_entry_size;
	uint64_t partition_start_lba;
	struct spdk_gpt_header *head = gpt->header;
	uint32_t crc32;

	num_partition_entries = from_le32(&head->num_partition_entries);
	if (num_partition_entries > SPDK_MAX_NUM_PARTITION_ENTRIES) {
		SPDK_ERRLOG("Num_partition_entries=%u which exceeds max=%u\n",
			    num_partition_entries, SPDK_MAX_NUM_PARTITION_ENTRIES);
		return -1;
	}

	partition_entry_size = from_le32(&head->size_of_partition_entry);
	if (partition_entry_size != sizeof(struct spdk_gpt_partition_entry)) {
		SPDK_ERRLOG("Partition_entry_size(%x) != expected(%lx)\n",
			    partition_entry_size, sizeof(struct spdk_gpt_partition_entry));
		return -1;
	}

	total_partition_size = num_partition_entries * partition_entry_size;
	partition_start_lba = from_le64(&head->partition_entry_lba);
	if ((total_partition_size + partition_start_lba * gpt->sector_size) > SPDK_GPT_BUFFER_SIZE) {
		SPDK_ERRLOG("Buffer size is not enough\n");
		return -1;
	}

	gpt->partitions = (struct spdk_gpt_partition_entry *)(gpt->buf +
			  partition_start_lba * gpt->sector_size);

	crc32 = spdk_gpt_crc32((uint8_t *)gpt->partitions, total_partition_size, ~0);

	if (crc32 != from_le32(&head->partition_entry_array_crc32)) {
		SPDK_ERRLOG("GPT partition entry array crc32 did not match\n");
		return -1;
	}

	return 0;
}

static int
spdk_gpt_lba_range_check(struct spdk_gpt_header *head, uint64_t lba_end)
{
	uint64_t usable_lba_start, usable_lba_end;

	usable_lba_start = from_le64(&head->first_usable_lba);
	usable_lba_end = from_le64(&head->last_usable_lba);

	if (usable_lba_end < usable_lba_start) {
		SPDK_ERRLOG("Head's usable_lba_end(%" PRIu64 ") < usable_lba_start(%" PRIu64 ")\n",
			    usable_lba_end, usable_lba_start);
		return -1;
	}

	if (usable_lba_end > lba_end) {
		SPDK_ERRLOG("Head's usable_lba_end(%" PRIu64 ") > lba_end(%" PRIu64 ")\n",
			    usable_lba_end, lba_end);
		return -1;
	}

	if ((usable_lba_start < GPT_PRIMARY_PARTITION_TABLE_LBA) &&
	    (GPT_PRIMARY_PARTITION_TABLE_LBA < usable_lba_end)) {
		SPDK_ERRLOG("Head lba is not in the usable range\n");
		return -1;
	}

	return 0;
}

static int
spdk_gpt_read_header(struct spdk_gpt *gpt)
{
	uint32_t head_size;
	uint32_t new_crc, original_crc;
	struct spdk_gpt_header *head;

	head = (struct spdk_gpt_header *)(gpt->buf + GPT_PRIMARY_PARTITION_TABLE_LBA * gpt->sector_size);
	head_size = from_le32(&head->header_size);
	if (head_size < sizeof(*head) || head_size > gpt->sector_size) {
		SPDK_ERRLOG("head_size=%u\n", head_size);
		return -1;
	}

	original_crc = from_le32(&head->header_crc32);
	head->header_crc32 = 0;
	new_crc = spdk_gpt_crc32((uint8_t *)head, from_le32(&head->header_size), ~0);
	/* restore header crc32 */
	to_le32(&head->header_crc32, original_crc);

	if (new_crc != original_crc) {
		SPDK_ERRLOG("head crc32 does not match, provided=%u, caculated=%u\n",
			    original_crc, new_crc);
		return -1;
	}

	if (memcmp(SPDK_GPT_SIGNATURE, head->gpt_signature,
		   sizeof(head->gpt_signature))) {
		SPDK_ERRLOG("signature did not match\n");
		return -1;
	}

	if (spdk_gpt_lba_range_check(head, gpt->lba_end)) {
		SPDK_ERRLOG("lba range check error\n");
		return -1;
	}

	gpt->header = head;
	return 0;
}

static int
spdk_gpt_check_mbr(struct spdk_gpt *gpt)
{
	int i, primary_partition = 0;
	uint32_t total_lba_size = 0, ret = 0, expected_start_lba;
	struct spdk_mbr *mbr;

	mbr = (struct spdk_mbr *)gpt->buf;
	if (from_le16(&mbr->mbr_signature) != SPDK_MBR_SIGNATURE) {
		SPDK_TRACELOG(SPDK_TRACE_GPT_PARSE, "Signature mismatch, provided=%x,"
			      "expected=%x\n", from_le16(&mbr->disk_signature),
			      SPDK_MBR_SIGNATURE);
		return -1;
	}

	to_le32(&expected_start_lba, GPT_PRIMARY_PARTITION_TABLE_LBA);
	if (mbr->partitions[0].start_lba != expected_start_lba) {
		SPDK_TRACELOG(SPDK_TRACE_GPT_PARSE, "start lba mismatch, provided=%u, expected=%u\n",
			      mbr->partitions[0].start_lba, expected_start_lba);
		return -1;
	}

	for (i = 0; i < PRIMARY_PARTITION_NUMBER; i++) {
		if (mbr->partitions[i].os_type == SPDK_MBR_OS_TYPE_GPT_PROTECTIVE) {
			primary_partition = i;
			ret = GPT_PROTECTIVE_MBR;
			break;
		}
	}

	if (ret == GPT_PROTECTIVE_MBR) {
		total_lba_size = from_le32(&mbr->partitions[primary_partition].size_lba);
		if ((total_lba_size != ((uint32_t) gpt->total_sectors - 1)) &&
		    (total_lba_size != 0xFFFFFFFF)) {
			SPDK_ERRLOG("GPT Primary MBR size does not equal: (record_size %u != actual_size %u)!\n",
				    total_lba_size, (uint32_t) gpt->total_sectors - 1);
			return -1;
		}
	} else {
		SPDK_ERRLOG("Currently only support GPT Protective MBR format\n");
		return -1;
	}

	gpt->mbr = mbr;
	return 0;
}

int
spdk_gpt_parse(struct spdk_gpt *gpt)
{
	int rc;

	if (!gpt || !gpt->buf) {
		SPDK_ERRLOG("Gpt and the related buffer should not be NULL\n");
		return -1;
	}

	rc = spdk_gpt_check_mbr(gpt);
	if (rc) {
		SPDK_TRACELOG(SPDK_TRACE_GPT_PARSE, "Failed to detect gpt in MBR\n");
		return rc;
	}

	rc = spdk_gpt_read_header(gpt);
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt header\n");
		return rc;
	}

	rc = spdk_gpt_read_partitions(gpt);
	if (rc) {
		SPDK_ERRLOG("Failed to read gpt partitions\n");
		return rc;
	}

	return 0;
}

SPDK_LOG_REGISTER_TRACE_FLAG("gpt_parse", SPDK_TRACE_GPT_PARSE)
