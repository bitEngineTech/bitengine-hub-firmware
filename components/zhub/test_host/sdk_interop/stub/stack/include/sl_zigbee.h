#pragma once
#include "stack/include/sl_zigbee_types.h"
sli_buffer_manager_buffer_t sli_legacy_buffer_manager_allocate_indirect_buffer(uint8_t *contents, void *freePtr, uint16_t length);
uint8_t  sl_legacy_buffer_manager_message_buffer_length(sli_buffer_manager_buffer_t b);
void     sl_legacy_buffer_manager_set_message_buffer_length(sli_buffer_manager_buffer_t b, uint8_t len);
uint8_t *sl_legacy_buffer_manager_message_buffer_contents(sli_buffer_manager_buffer_t b);
void     sl_legacy_buffer_manager_copy_to_linked_buffers(const uint8_t *from, sli_buffer_manager_buffer_t b, uint8_t start, uint8_t n);
void     sl_legacy_buffer_manager_set_linked_buffers_byte(sli_buffer_manager_buffer_t b, uint8_t i, uint8_t v);
uint8_t  sl_legacy_buffer_manager_get_linked_buffers_byte(sli_buffer_manager_buffer_t b, uint8_t i);
uint8_t *sl_legacy_buffer_manager_get_linked_buffers_pointer(sli_buffer_manager_buffer_t b, uint8_t i);
void     sl_legacy_buffer_manager_hold_message_buffer(sli_buffer_manager_buffer_t b);
void     sl_legacy_buffer_manager_release_message_buffer(sli_buffer_manager_buffer_t b);
bool     sli_legacy_packet_buffer_queue_is_empty(sli_zigbee_message_buffer_queue_t *q);
void     sli_legacy_packet_buffer_queue_add(sli_zigbee_message_buffer_queue_t *q, sli_buffer_manager_buffer_t b);
sli_buffer_manager_buffer_t sli_legacy_packet_buffer_queue_remove_head(sli_zigbee_message_buffer_queue_t *q);
sli_buffer_manager_buffer_t sli_legacy_buffer_manager_buffer_queue_head(sli_zigbee_message_buffer_queue_t *q);
sli_buffer_manager_buffer_t sli_legacy_buffer_manager_buffer_queue_next(sli_zigbee_message_buffer_queue_t *q, sli_buffer_manager_buffer_t b);
uint8_t  sli_zigbee_message_buffer_queue_length(sli_zigbee_message_buffer_queue_t *q);
#define sli_legacy_buffer_manager_mark_buffer(p) ((void)(p))
