/* Shared between ndp_agent.c and ndp_agent_fs.c. Not part of the public API. */
#ifndef NDP_AGENT_INTERNAL_H
#define NDP_AGENT_INTERNAL_H

#include "ndp/ndp_agent.h"
#include "ndp/ndp_tlv.h"

/* Builds an ERR frame into `out`; returns its length (0 if it does not fit). */
size_t ndp_agent_error(uint8_t *out, size_t cap, const ndp_header *req, uint16_t status, const char *detail,
                       int with_supported);

/* Wraps the TLV payload already written at out + NDP_HEADER_SIZE into a frame of the given kind. */
size_t ndp_agent_finish(uint8_t *out, size_t cap, const ndp_header *req, uint8_t kind, uint16_t status,
                        const ndp_tlv_w *w);

/* FS_LIST / FS_STAT / FS_READ (requires cfg.fs). */
size_t ndp_agent_fs_handle(ndp_agent *a, const ndp_header *req, const uint8_t *payload, uint8_t *out, size_t cap);
size_t ndp_agent_fs_next_frame(ndp_agent *a, uint8_t *out, size_t cap);
void ndp_agent_fs_close(ndp_agent *a);

/* FS_WRITE / FS_MKDIR / upload frames (ndp_agent_write.c; require cfg.fs->file_create). */
size_t ndp_agent_write_start(ndp_agent *a, const ndp_header *req, const uint8_t *payload, uint8_t *out, size_t cap);
size_t ndp_agent_delete(ndp_agent *a, const ndp_header *req, const uint8_t *payload, uint8_t *out, size_t cap);
size_t ndp_agent_mkdir(ndp_agent *a, const ndp_header *req, const uint8_t *payload, uint8_t *out, size_t cap);
size_t ndp_agent_upload_frame(ndp_agent *a, const ndp_header *hdr, const uint8_t *payload, uint8_t *out, size_t cap);
void ndp_agent_upload_abort(ndp_agent *a, int discard);

#endif
