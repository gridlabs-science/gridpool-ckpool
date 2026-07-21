#include "config.h"
#include "gridpool_adapter.h"
#include "yyjson.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define GRIDPOOL_IPC_SCHEMA 1

static bool write_all(int fd, const void *buffer, size_t length)
{
	const unsigned char *cursor = buffer;

	while (length) {
		ssize_t written = write(fd, cursor, length);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		cursor += written;
		length -= written;
	}
	return true;
}

static bool read_all(int fd, void *buffer, size_t length)
{
	unsigned char *cursor = buffer;

	while (length) {
		ssize_t received = read(fd, cursor, length);

		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0)
			return false;
		cursor += received;
		length -= received;
	}
	return true;
}

static char *ipc_request(const char *socket_path, size_t maximum_message_bytes, const char *request)
{
	struct sockaddr_un address = {0};
	uint32_t request_length, response_length;
	char *response = NULL;
	int fd = -1;

	if (!socket_path || !request || !maximum_message_bytes ||
	    strlen(socket_path) >= sizeof(address.sun_path) ||
	    strlen(request) > maximum_message_bytes)
		return NULL;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return NULL;
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)))
		goto out;
	request_length = htonl((uint32_t)strlen(request));
	if (!write_all(fd, &request_length, sizeof(request_length)) ||
	    !write_all(fd, request, strlen(request)) ||
	    !read_all(fd, &response_length, sizeof(response_length)))
		goto out;
	response_length = ntohl(response_length);
	if (!response_length || response_length > maximum_message_bytes)
		goto out;
	response = calloc(response_length + 1, 1);
	if (!response || !read_all(fd, response, response_length)) {
		free(response);
		response = NULL;
	}
out:
	if (fd >= 0)
		close(fd);
	return response;
}

static bool parse_ok_data(char *response, yyjson_doc **document, yyjson_val **data)
{
	yyjson_val *root, *status;

	*document = yyjson_read(response, strlen(response), YYJSON_READ_STOP_WHEN_DONE);
	if (!*document)
		return false;
	root = yyjson_doc_get_root(*document);
	status = yyjson_obj_get(root, "status");
	if (!yyjson_is_str(status) || strcmp(yyjson_get_str(status), "ok"))
		return false;
	*data = yyjson_obj_get(root, "data");
	return yyjson_is_obj(*data);
}

static bool read_compact_size(const unsigned char *data, size_t length, size_t *offset, uint64_t *value)
{
	unsigned char marker;
	size_t width, i;

	if (*offset >= length)
		return false;
	marker = data[(*offset)++];
	if (marker < 0xfd) {
		*value = marker;
		return true;
	}
	width = marker == 0xfd ? 2 : marker == 0xfe ? 4 : 8;
	if (length - *offset < width)
		return false;
	*value = 0;
	for (i = 0; i < width; i++)
		*value |= (uint64_t)data[(*offset)++] << (8 * i);
	return true;
}

static bool parse_suffix(gridpool_plan_t *plan, const char *hex, uint32_t expected_outputs)
{
	uint64_t output_count, script_length, value;
	unsigned char *serialized;
	size_t length, offset = 0, suffix_offset, i;

	if (!hex || strlen(hex) % 2)
		return false;
	length = strlen(hex) / 2;
	serialized = calloc(length ? length : 1, 1);
	if (!serialized || !hex2bin(serialized, hex, length)) {
		free(serialized);
		return false;
	}
	if (!read_compact_size(serialized, length, &offset, &output_count) ||
	    output_count != expected_outputs) {
		free(serialized);
		return false;
	}
	suffix_offset = offset;
	for (i = 0; i < output_count; i++) {
		if (length - offset < 8) {
			free(serialized);
			return false;
		}
		value = 0;
		for (size_t j = 0; j < 8; j++)
			value |= (uint64_t)serialized[offset++] << (8 * j);
		if (UINT64_MAX - plan->suffix_value < value ||
		    !read_compact_size(serialized, length, &offset, &script_length) ||
		    script_length > length - offset) {
			free(serialized);
			return false;
		}
		plan->suffix_value += value;
		offset += (size_t)script_length;
	}
	if (offset != length) {
		free(serialized);
		return false;
	}
	plan->suffix_len = length - suffix_offset;
	plan->suffix = calloc(plan->suffix_len ? plan->suffix_len : 1, 1);
	if (!plan->suffix) {
		free(serialized);
		return false;
	}
	memcpy(plan->suffix, serialized + suffix_offset, plan->suffix_len);
	plan->suffix_outputs = expected_outputs;
	free(serialized);
	return true;
}

void gridpool_plan_clear(gridpool_plan_t *plan)
{
	if (!plan)
		return;
	free(plan->suffix);
	memset(plan, 0, sizeof(*plan));
}

bool gridpool_adapter_get_plan(const char *socket_path, size_t maximum_message_bytes,
			       gridpool_plan_t *plan)
{
	const char *plan_id, *snapshot_id, *parent_hash, *suffix_hex;
	yyjson_doc *document = NULL;
	yyjson_val *data, *value;
	char *response;
	bool result = false;

	gridpool_plan_clear(plan);
	response = ipc_request(socket_path, maximum_message_bytes,
		"{\"type\":\"get_plan\",\"schema_version\":1}");
	if (!response || !parse_ok_data(response, &document, &data))
		goto out;
	plan_id = yyjson_get_str(yyjson_obj_get(data, "planId"));
	snapshot_id = yyjson_get_str(yyjson_obj_get(data, "activeSnapshotId"));
	parent_hash = yyjson_get_str(yyjson_obj_get(data, "currentTipBlockHash"));
	suffix_hex = yyjson_get_str(yyjson_obj_get(data, "coinbaseTxOutputsHex"));
	value = yyjson_obj_get(data, "coinbaseOutputCount");
	if (!plan_id || strlen(plan_id) != GRIDPOOL_PLAN_ID_LEN || !snapshot_id ||
	    strlen(snapshot_id) > GRIDPOOL_SNAPSHOT_ID_MAX || !parent_hash ||
	    strlen(parent_hash) != 64 || !yyjson_is_uint(value))
		goto out;
	strcpy(plan->plan_id, plan_id);
	strcpy(plan->snapshot_id, snapshot_id);
	strcpy(plan->parent_hash, parent_hash);
	plan->minimum_reserve_difficulty = yyjson_get_num(
		yyjson_obj_get(data, "minimumDifficultyToEnterReserve"));
	plan->minimum_pulse_difficulty = yyjson_get_num(
		yyjson_obj_get(data, "minimumPulseDifficulty"));
	if (plan->minimum_pulse_difficulty < 1)
		goto out;
	if (!parse_suffix(plan, suffix_hex, (uint32_t)yyjson_get_uint(value)))
		goto out;
	plan->available = true;
	result = true;
out:
	if (!result)
		gridpool_plan_clear(plan);
	if (document)
		yyjson_doc_free(document);
	free(response);
	return result;
}

bool gridpool_adapter_fee_decision(const char *socket_path, size_t maximum_message_bytes,
				   const char *parent_hash, const char *payout_script_hex,
				   int64_t unix_seconds, bool *fee_active, int64_t *bucket)
{
	yyjson_mut_doc *request_doc;
	yyjson_mut_val *root;
	yyjson_doc *document = NULL;
	yyjson_val *data;
	char *request, *response = NULL;
	size_t request_length;
	bool result = false;

	request_doc = yyjson_mut_doc_new(NULL);
	root = yyjson_mut_obj(request_doc);
	yyjson_mut_doc_set_root(request_doc, root);
	yyjson_mut_obj_add_str(request_doc, root, "type", "fee_decision");
	yyjson_mut_obj_add_uint(request_doc, root, "schema_version", GRIDPOOL_IPC_SCHEMA);
	yyjson_mut_obj_add_str(request_doc, root, "parent_hash", parent_hash);
	yyjson_mut_obj_add_str(request_doc, root, "payout_script_hex", payout_script_hex);
	yyjson_mut_obj_add_sint(request_doc, root, "unix_seconds", unix_seconds);
	request = yyjson_mut_write(request_doc, 0, &request_length);
	response = ipc_request(socket_path, maximum_message_bytes, request);
	free(request);
	yyjson_mut_doc_free(request_doc);
	if (!response || !parse_ok_data(response, &document, &data))
		goto out;
	*fee_active = yyjson_is_true(yyjson_obj_get(data, "feeActive"));
	*bucket = yyjson_get_sint(yyjson_obj_get(data, "bucket"));
	result = true;
out:
	if (document)
		yyjson_doc_free(document);
	free(response);
	return result;
}

bool gridpool_adapter_submit_proof(const char *socket_path, size_t maximum_message_bytes,
				   const char *proof_json)
{
	char *request, *response;
	size_t length;
	bool result;

	if (!proof_json)
		return false;
	length = strlen(proof_json) + 64;
	request = calloc(length, 1);
	if (!request)
		return false;
	snprintf(request, length, "{\"type\":\"submit_proof\",\"schema_version\":1,\"proof\":%s}",
		 proof_json);
	response = ipc_request(socket_path, maximum_message_bytes, request);
	result = response && strstr(response, "\"status\":\"ok\"");
	free(response);
	free(request);
	return result;
}

bool gridpool_adapter_record_share(const char *socket_path, size_t maximum_message_bytes,
				   const char *channel_id, const char *payout_address,
				   const char *username, bool accepted, double difficulty,
				   bool fee_work, int64_t observed_unix_ms)
{
	yyjson_mut_doc *document;
	yyjson_mut_val *root;
	char *request, *response;
	bool result;

	if (!channel_id || !payout_address || !username)
		return false;
	document = yyjson_mut_doc_new(NULL);
	root = yyjson_mut_obj(document);
	yyjson_mut_doc_set_root(document, root);
	yyjson_mut_obj_add_str(document, root, "type", "record_share");
	yyjson_mut_obj_add_uint(document, root, "schema_version", GRIDPOOL_IPC_SCHEMA);
	yyjson_mut_obj_add_str(document, root, "channel_id", channel_id);
	yyjson_mut_obj_add_str(document, root, "payout_address", payout_address);
	yyjson_mut_obj_add_str(document, root, "username", username);
	yyjson_mut_obj_add_bool(document, root, "accepted", accepted);
	yyjson_mut_obj_add_real(document, root, "difficulty", difficulty);
	yyjson_mut_obj_add_bool(document, root, "fee_work", fee_work);
	yyjson_mut_obj_add_sint(document, root, "observed_unix_ms", observed_unix_ms);
	request = yyjson_mut_write(document, 0, NULL);
	response = ipc_request(socket_path, maximum_message_bytes, request);
	result = response && strstr(response, "\"status\":\"ok\"");
	free(response);
	free(request);
	yyjson_mut_doc_free(document);
	return result;
}

bool gridpool_password_enabled(const char *password)
{
	static const char token[] = "USE_GRIDPOOL_SPLIT";
	const char *start, *end;
	size_t length, i;

	if (!password)
		return false;
	for (start = password; *start;) {
		while (*start && (*start == ',' || *start == ';' || isspace((unsigned char)*start)))
			start++;
		end = start;
		while (*end && *end != ',' && *end != ';' && !isspace((unsigned char)*end))
			end++;
		length = (size_t)(end - start);
		if (length == sizeof(token) - 1) {
			for (i = 0; i < length; i++)
				if (toupper((unsigned char)start[i]) != token[i])
					break;
			if (i == length)
				return true;
		}
		start = end;
	}
	return false;
}
