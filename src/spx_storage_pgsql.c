/* SPX - A simple profiler for PHP
 * Copyright (C) 2017-2025 Sylvain Lassaut <NoiseByNorthwest@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#ifdef SPX_STORAGE_POSTGRESQL_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libpq-fe.h>

#include "spx_storage.h"
#include "spx_utils.h"
#include "spx_php.h"
#include "spx_metric.h"
#include "spx_str_builder.h"
#include "spx_output_stream.h"

typedef struct {
    PGconn * conn;
    char * connection_string;
    int report_id;
    spx_output_stream_t * current_output;
    spx_str_builder_t * str_builder;
    char * temp_file_path;
    int in_transaction;
} spx_storage_pgsql_private_t;

static int pgsql_init(spx_storage_t * storage);
static int pgsql_destroy(spx_storage_t * storage);
static int pgsql_begin_report(spx_storage_t * storage, const spx_storage_metadata_t * metadata);
static int pgsql_save_events(spx_storage_t * storage, const char * key, const spx_storage_event_buffer_t * events, const int * enabled_metrics);
static int pgsql_save_functions(spx_storage_t * storage, const char * key, const spx_storage_function_table_t * functions);
static int pgsql_finalize_report(spx_storage_t * storage, const char * key, const spx_storage_metadata_t * metadata);
static size_t pgsql_list_reports(spx_storage_t * storage, void (*callback)(const char * metadata_json, size_t count));
static int pgsql_get_report_metadata(spx_storage_t * storage, const char * key, char ** metadata_json);
static int pgsql_get_report_events(spx_storage_t * storage, const char * key, char ** events_data, size_t * size);

static int connect_to_database(spx_storage_pgsql_private_t * priv);
static int insert_report_metadata(spx_storage_pgsql_private_t * priv, const spx_storage_metadata_t * metadata);
static int update_report_with_blobs(spx_storage_pgsql_private_t * priv, const char * key, const spx_storage_metadata_t * metadata, const unsigned char * events_data, size_t events_size);
static char * build_metadata_json(const spx_storage_metadata_t * metadata);
static char * escape_json_string(const char * str);
static void parse_enabled_metrics_csv(const char * csv_string, int * enabled_metrics);
static void extract_metadata_from_result(PGresult * res, int row, spx_storage_metadata_t * metadata);

static const spx_storage_interface_t pgsql_interface = {
    .init = pgsql_init,
    .destroy = pgsql_destroy,
    .begin_report = pgsql_begin_report,
    .save_events = pgsql_save_events,
    .save_functions = pgsql_save_functions,
    .finalize_report = pgsql_finalize_report,
    .list_reports = pgsql_list_reports,
    .get_report_metadata = pgsql_get_report_metadata,
    .get_report_events = pgsql_get_report_events,
};

spx_storage_t * spx_storage_create_postgresql(const char * connection_string)
{
    spx_storage_t * storage = malloc(sizeof(*storage));
    if (!storage) {
        return NULL;
    }

    spx_storage_pgsql_private_t * priv = malloc(sizeof(*priv));
    if (!priv) {
        free(storage);
        return NULL;
    }

    priv->connection_string = strdup(connection_string);
    if (!priv->connection_string) {
        free(priv);
        free(storage);
        return NULL;
    }

    priv->conn = NULL;
    priv->report_id = 0;
    priv->current_output = NULL;
    priv->str_builder = NULL;
    priv->temp_file_path = NULL;
    priv->in_transaction = 0;

    storage->type = SPX_STORAGE_TYPE_POSTGRESQL;
    storage->interface = &pgsql_interface;
    storage->private_data = priv;

    if (storage->interface->init && storage->interface->init(storage) != 0) {
        spx_storage_destroy(storage);
        return NULL;
    }

    return storage;
}

static int pgsql_init(spx_storage_t * storage)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;

    if (connect_to_database(priv) != 0) {
        return -1;
    }


    return 0;
}

static int pgsql_destroy(spx_storage_t * storage)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;

    if (priv->conn) {
        PQfinish(priv->conn);
    }

    if (priv->connection_string) {
        free(priv->connection_string);
    }

    if (priv->current_output) {
        spx_output_stream_close(priv->current_output);
    }

    if (priv->str_builder) {
        spx_str_builder_destroy(priv->str_builder);
    }

    if (priv->temp_file_path) {
        unlink(priv->temp_file_path);
        free(priv->temp_file_path);
    }

    free(priv);

    return 0;
}

static int pgsql_begin_report(spx_storage_t * storage, const spx_storage_metadata_t * metadata)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;

    priv->temp_file_path = malloc(PATH_MAX);
    if (!priv->temp_file_path) {
        return -1;
    }
    snprintf(priv->temp_file_path, PATH_MAX, "/tmp/spx-pgsql-%s.tmp", metadata->key);

    priv->current_output = spx_output_stream_open(priv->temp_file_path, 1);
    if (!priv->current_output) {
        free(priv->temp_file_path);
        priv->temp_file_path = NULL;
        return -1;
    }

    priv->str_builder = spx_str_builder_create(8 * 1024);
    if (!priv->str_builder) {
        spx_output_stream_close(priv->current_output);
        priv->current_output = NULL;
        free(priv->temp_file_path);
        priv->temp_file_path = NULL;
        return -1;
    }

    spx_output_stream_print(priv->current_output, "[events]\n");

    PGresult * res = PQexec(priv->conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        spx_php_log_notice("SPX: Failed to begin transaction: %s", PQerrorMessage(priv->conn));
        PQclear(res);
        return -1;
    }
    PQclear(res);
    priv->in_transaction = 1;

    int result = insert_report_metadata(priv, metadata);
    if (result != 0) {
        res = PQexec(priv->conn, "ROLLBACK");
        PQclear(res);
        priv->in_transaction = 0;
    }

    return result;
}

static int pgsql_save_events(spx_storage_t * storage, const char * key, const spx_storage_event_buffer_t * events, const int * enabled_metrics)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;
    return spx_storage_save_events(priv->current_output, priv->str_builder, events, enabled_metrics);
}

static int pgsql_save_functions(spx_storage_t * storage, const char * key, const spx_storage_function_table_t * functions)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;
    return spx_storage_save_functions(priv->current_output, functions);
}

static int pgsql_finalize_report(spx_storage_t * storage, const char * key, const spx_storage_metadata_t * metadata)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;
    int result = -1;

    if (!priv->current_output || !priv->temp_file_path) {
        return -1;
    }

    spx_output_stream_close(priv->current_output);
    priv->current_output = NULL;

    FILE * temp_file = fopen(priv->temp_file_path, "rb");
    if (!temp_file) {
        goto cleanup_temp_file;
    }

    fseek(temp_file, 0, SEEK_END);
    long file_size = ftell(temp_file);
    fseek(temp_file, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(temp_file);
        goto cleanup_temp_file;
    }

    char * file_data = malloc(file_size);
    if (!file_data) {
        fclose(temp_file);
        goto cleanup_temp_file;
    }

    size_t bytes_read = fread(file_data, 1, file_size, temp_file);
    fclose(temp_file);

    if (bytes_read != (size_t)file_size) {
        free(file_data);
        goto cleanup_temp_file;
    }

    result = update_report_with_blobs(priv, key, metadata, (const unsigned char*)file_data, file_size);

    if (priv->in_transaction) {
        PGresult * res;
        if (result == 0) {
            res = PQexec(priv->conn, "COMMIT");
            if (PQresultStatus(res) != PGRES_COMMAND_OK) {
                spx_php_log_notice("SPX: Failed to commit transaction: %s", PQerrorMessage(priv->conn));
                result = -1;
            }
        } else {
            res = PQexec(priv->conn, "ROLLBACK");
            spx_php_log_notice("SPX: Failed to store report %s, rolling back", key);
        }
        PQclear(res);
        priv->in_transaction = 0;
    }

    free(file_data);

cleanup_temp_file:
    if (priv->in_transaction) {
        PGresult * res = PQexec(priv->conn, "ROLLBACK");
        PQclear(res);
        priv->in_transaction = 0;
        result = -1;
    }

    if (priv->temp_file_path) {
        unlink(priv->temp_file_path);
        free(priv->temp_file_path);
        priv->temp_file_path = NULL;
    }

    if (priv->str_builder) {
        spx_str_builder_destroy(priv->str_builder);
        priv->str_builder = NULL;
    }

    priv->report_id = 0;

    return result;
}

static size_t pgsql_list_reports(spx_storage_t * storage, void (*callback)(const char * metadata_json, size_t count))
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;

    const char * query = "SELECT key, exec_ts, host_name, process_pid, process_tid, process_pwd, "
                         "is_cli, cli_command_line, http_request_uri, http_method, http_host, "
                         "custom_metadata_str, wall_time_ms, peak_memory_usage, called_function_count, "
                         "call_count, recorded_call_count, enabled_metrics "
                         "FROM spx_reports ORDER BY exec_ts DESC";

    PGresult * res = PQexec(priv->conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return 0;
    }

    int ntuples = PQntuples(res);
    int i;
    for (i = 0; i < ntuples; i++) {
        spx_storage_metadata_t metadata;
        extract_metadata_from_result(res, i, &metadata);

        char * metadata_json = build_metadata_json(&metadata);
        if (metadata_json) {
            callback(metadata_json, i);
            free(metadata_json);
        }
    }

    PQclear(res);
    return ntuples;
}

static int pgsql_get_report_metadata(spx_storage_t * storage, const char * key, char ** metadata_json)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;

    char query[512];
    char * escaped_key = PQescapeLiteral(priv->conn, key, strlen(key));
    if (!escaped_key) {
        return -1;
    }

    snprintf(
        query,
        sizeof(query),
        "SELECT key, exec_ts, host_name, process_pid, process_tid, process_pwd, "
        "is_cli, cli_command_line, http_request_uri, http_method, http_host, "
        "custom_metadata_str, wall_time_ms, peak_memory_usage, called_function_count, "
        "call_count, recorded_call_count, enabled_metrics "
        "FROM spx_reports WHERE key = %s",
        escaped_key
    );

    PQfreemem(escaped_key);

    PGresult * res = PQexec(priv->conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return -1;
    }

    spx_storage_metadata_t metadata;
    extract_metadata_from_result(res, 0, &metadata);

    *metadata_json = build_metadata_json(&metadata);

    PQclear(res);
    return *metadata_json ? 0 : -1;
}

static int pgsql_get_report_events(spx_storage_t * storage, const char * key, char ** events_data, size_t * size)
{
    spx_storage_pgsql_private_t * priv = (spx_storage_pgsql_private_t *) storage->private_data;

    char query[256];
    char * escaped_key = PQescapeLiteral(priv->conn, key, strlen(key));
    if (!escaped_key) {
        spx_php_log_notice("SPX: Failed to escape key");
        return -1;
    }

    snprintf(query, sizeof(query), "SELECT events_data FROM spx_reports WHERE key = %s", escaped_key);
    PQfreemem(escaped_key);

    PGresult * res = PQexecParams(
        priv->conn,
        query,
        0, NULL, NULL, NULL, NULL,
        1 // binary result format
    );

    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        spx_php_log_notice("SPX: Report not found or query failed for key: %s", key);
        PQclear(res);
        return -1;
    }

    if (PQgetisnull(res, 0, 0)) {
        spx_php_log_notice("SPX: Report events_data is NULL for key: %s", key);
        PQclear(res);
        return -1;
    }

    const char * gzipped_data = PQgetvalue(res, 0, 0);
    size_t gzipped_size = PQgetlength(res, 0, 0);

    if (gzipped_size == 0) {
        spx_php_log_notice("SPX: Report data is empty for key: %s", key);
        PQclear(res);
        return -1;
    }

    *events_data = malloc(gzipped_size);
    if (!*events_data) {
        spx_php_log_notice("SPX: Failed to allocate memory for events data");
        PQclear(res);
        return -1;
    }

    memcpy(*events_data, gzipped_data, gzipped_size);
    *size = gzipped_size;

    PQclear(res);

    return 0;
}

static void extract_metadata_from_result(PGresult * res, int row, spx_storage_metadata_t * metadata)
{
    metadata->key = PQgetvalue(res, row, 0);
    metadata->exec_ts = atol(PQgetvalue(res, row, 1));
    metadata->hostname = PQgetisnull(res, row, 2) ? "n/a" : PQgetvalue(res, row, 2);
    metadata->process_pid = atoi(PQgetvalue(res, row, 3));
    metadata->process_tid = PQgetisnull(res, row, 4) ? 0 : atoi(PQgetvalue(res, row, 4));
    metadata->process_pwd = PQgetisnull(res, row, 5) ? "n/a" : PQgetvalue(res, row, 5);
    metadata->cli = strcmp(PQgetvalue(res, row, 6), "t") == 0 ? 1 : 0;
    metadata->cli_command_line = PQgetisnull(res, row, 7) ? "n/a" : PQgetvalue(res, row, 7);
    metadata->http_request_uri = PQgetisnull(res, row, 8) ? "n/a" : PQgetvalue(res, row, 8);
    metadata->http_method = PQgetisnull(res, row, 9) ? "n/a" : PQgetvalue(res, row, 9);
    metadata->http_host = PQgetisnull(res, row, 10) ? "n/a" : PQgetvalue(res, row, 10);
    metadata->custom_metadata_str = PQgetisnull(res, row, 11) ? NULL : PQgetvalue(res, row, 11);
    metadata->wall_time_ms = PQgetisnull(res, row, 12) ? 0 : atol(PQgetvalue(res, row, 12));
    metadata->peak_memory_usage = PQgetisnull(res, row, 13) ? 0 : atol(PQgetvalue(res, row, 13));
    metadata->called_function_count = PQgetisnull(res, row, 14) ? 0 : atol(PQgetvalue(res, row, 14));
    metadata->call_count = PQgetisnull(res, row, 15) ? 0 : atol(PQgetvalue(res, row, 15));
    metadata->recorded_call_count = PQgetisnull(res, row, 16) ? 0 : atol(PQgetvalue(res, row, 16));

    const char * enabled_metrics_str = PQgetisnull(res, row, 17) ? "" : PQgetvalue(res, row, 17);
    parse_enabled_metrics_csv(enabled_metrics_str, metadata->enabled_metrics);
}

static int connect_to_database(spx_storage_pgsql_private_t * priv)
{
    priv->conn = PQconnectdb(priv->connection_string);
    if (PQstatus(priv->conn) != CONNECTION_OK) {
        PQfinish(priv->conn);
        priv->conn = NULL;
        return -1;
    }

    return 0;
}


static int insert_report_metadata(spx_storage_pgsql_private_t * priv, const spx_storage_metadata_t * metadata)
{
    char query[2048];
    char * escaped_key = PQescapeLiteral(priv->conn, metadata->key, strlen(metadata->key));
    char * escaped_hostname = PQescapeLiteral(priv->conn, metadata->hostname, strlen(metadata->hostname));
    char * escaped_pwd = PQescapeLiteral(priv->conn, metadata->process_pwd, strlen(metadata->process_pwd));
    char * escaped_cli_cmd = PQescapeLiteral(priv->conn, metadata->cli_command_line, strlen(metadata->cli_command_line));
    char * escaped_uri = PQescapeLiteral(priv->conn, metadata->http_request_uri, strlen(metadata->http_request_uri));
    char * escaped_method = PQescapeLiteral(priv->conn, metadata->http_method, strlen(metadata->http_method));
    char * escaped_host = PQescapeLiteral(priv->conn, metadata->http_host, strlen(metadata->http_host));

    if (!escaped_key || !escaped_hostname || !escaped_pwd || !escaped_cli_cmd ||
        !escaped_uri || !escaped_method || !escaped_host) {
        spx_php_log_notice("SPX: Failed to escape metadata fields");
        if (escaped_key) PQfreemem(escaped_key);
        if (escaped_hostname) PQfreemem(escaped_hostname);
        if (escaped_pwd) PQfreemem(escaped_pwd);
        if (escaped_cli_cmd) PQfreemem(escaped_cli_cmd);
        if (escaped_uri) PQfreemem(escaped_uri);
        if (escaped_method) PQfreemem(escaped_method);
        if (escaped_host) PQfreemem(escaped_host);
        return -1;
    }

    snprintf(
        query,
        sizeof(query),
        "INSERT INTO spx_reports (key, exec_ts, host_name, process_pid, process_tid, "
        "process_pwd, is_cli, cli_command_line, http_request_uri, http_method, http_host) "
        "VALUES (%s, %zu, %s, %d, %d, %s, %s, %s, %s, %s, %s) RETURNING id",
        escaped_key, metadata->exec_ts, escaped_hostname, metadata->process_pid, metadata->process_tid,
        escaped_pwd, metadata->cli ? "true" : "false", escaped_cli_cmd, escaped_uri, escaped_method, escaped_host
    );

    PQfreemem(escaped_key);
    PQfreemem(escaped_hostname);
    PQfreemem(escaped_pwd);
    PQfreemem(escaped_cli_cmd);
    PQfreemem(escaped_uri);
    PQfreemem(escaped_method);
    PQfreemem(escaped_host);

    PGresult * res = PQexec(priv->conn, query);
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) == 0) {
        PQclear(res);
        return -1;
    }

    priv->report_id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    return 0;
}


static char * build_metadata_json(const spx_storage_metadata_t * metadata)
{
    size_t buffer_size = 8192;
    char * json = malloc(buffer_size);
    if (!json) {
        return NULL;
    }

    size_t pos = 0;

    #define SAFE_APPEND(...) do { \
        int written = snprintf(json + pos, buffer_size - pos, __VA_ARGS__); \
        if (written < 0 || (size_t)written >= buffer_size - pos) { \
            size_t new_size = buffer_size * 2; \
            char * new_json = realloc(json, new_size); \
            if (!new_json) { \
                free(json); \
                return NULL; \
            } \
            json = new_json; \
            buffer_size = new_size; \
            written = snprintf(json + pos, buffer_size - pos, __VA_ARGS__); \
            if (written < 0 || (size_t)written >= buffer_size - pos) { \
                free(json); \
                return NULL; \
            } \
        } \
        pos += written; \
    } while(0)

    SAFE_APPEND("{\n");

    char * escaped_key = escape_json_string(metadata->key);
    SAFE_APPEND("  \"key\": \"%s\",\n", escaped_key ? escaped_key : "");
    if (escaped_key) free(escaped_key);

    SAFE_APPEND("  \"exec_ts\": %zu,\n", metadata->exec_ts);

    char * escaped_hostname = escape_json_string(metadata->hostname);
    SAFE_APPEND("  \"host_name\": \"%s\",\n", escaped_hostname ? escaped_hostname : "");
    if (escaped_hostname) free(escaped_hostname);

    SAFE_APPEND("  \"process_pid\": %d,\n", metadata->process_pid);
    SAFE_APPEND("  \"process_tid\": %d,\n", metadata->process_tid);

    char * escaped_pwd = escape_json_string(metadata->process_pwd);
    SAFE_APPEND("  \"process_pwd\": \"%s\",\n", escaped_pwd ? escaped_pwd : "");
    if (escaped_pwd) free(escaped_pwd);

    SAFE_APPEND("  \"cli\": %d,\n", metadata->cli);

    char * escaped_cli_cmd = escape_json_string(metadata->cli_command_line);
    SAFE_APPEND("  \"cli_command_line\": \"%s\",\n", escaped_cli_cmd ? escaped_cli_cmd : "");
    if (escaped_cli_cmd) free(escaped_cli_cmd);

    char * escaped_uri = escape_json_string(metadata->http_request_uri);
    SAFE_APPEND("  \"http_request_uri\": \"%s\",\n", escaped_uri ? escaped_uri : "");
    if (escaped_uri) free(escaped_uri);

    char * escaped_method = escape_json_string(metadata->http_method);
    SAFE_APPEND("  \"http_method\": \"%s\",\n", escaped_method ? escaped_method : "");
    if (escaped_method) free(escaped_method);

    char * escaped_host = escape_json_string(metadata->http_host);
    SAFE_APPEND("  \"http_host\": \"%s\",\n", escaped_host ? escaped_host : "");
    if (escaped_host) free(escaped_host);

    if (metadata->custom_metadata_str) {
        char * escaped = escape_json_string(metadata->custom_metadata_str);
        SAFE_APPEND("  \"custom_metadata_str\": \"%s\",\n", escaped ? escaped : "");
        if (escaped) free(escaped);
    } else {
        SAFE_APPEND("  \"custom_metadata_str\": null,\n");
    }

    SAFE_APPEND("  \"wall_time_ms\": %zu,\n", metadata->wall_time_ms);
    SAFE_APPEND("  \"peak_memory_usage\": %zu,\n", metadata->peak_memory_usage);
    SAFE_APPEND("  \"called_function_count\": %zu,\n", metadata->called_function_count);
    SAFE_APPEND("  \"call_count\": %zu,\n", metadata->call_count);
    SAFE_APPEND("  \"recorded_call_count\": %zu,\n", metadata->recorded_call_count);

    SAFE_APPEND("  \"enabled_metrics\": [\n");
    int first = 1;
    size_t i;
    for (i = 0; i < SPX_METRIC_COUNT; i++) {
        if (!metadata->enabled_metrics[i]) {
            continue;
        }

        if (!first) {
            SAFE_APPEND(",\n");
        }
        first = 0;

        SAFE_APPEND("    \"%s\"", spx_metric_info[i].key);
    }
    SAFE_APPEND("\n  ]\n");
    SAFE_APPEND("}");

    #undef SAFE_APPEND

    return json;
}

static int update_report_with_blobs(spx_storage_pgsql_private_t * priv, const char * key, const spx_storage_metadata_t * metadata, const unsigned char * events_data, size_t events_size)
{
    char query[4096];
    char * escaped_key = PQescapeLiteral(priv->conn, key, strlen(key));

    if (!escaped_key) {
        return -1;
    }

    char enabled_metrics_str[512];
    char * pos = enabled_metrics_str;
    char * end = enabled_metrics_str + sizeof(enabled_metrics_str) - 1;
    int first = 1;
    size_t i;

    for (i = 0; i < SPX_METRIC_COUNT && pos < end; i++) {
        if (metadata->enabled_metrics[i]) {
            if (!first && pos < end) {
                *pos++ = ',';
            }
            int written = snprintf(pos, end - pos, "%zu", i);
            if (written > 0 && pos + written <= end) {
                pos += written;
            }
            first = 0;
        }
    }
    *pos = '\0';

    char * escaped_metrics = PQescapeLiteral(priv->conn, enabled_metrics_str, strlen(enabled_metrics_str));
    if (!escaped_metrics) {
        PQfreemem(escaped_key);
        return -1;
    }

    snprintf(
        query,
        sizeof(query),
        "UPDATE spx_reports SET "
        "wall_time_ms = %zu, "
        "peak_memory_usage = %zu, "
        "called_function_count = %zu, "
        "call_count = %zu, "
        "recorded_call_count = %zu, "
        "enabled_metrics = %s, "
        "events_data = $1 "
        "WHERE key = %s",
        metadata->wall_time_ms,
        metadata->peak_memory_usage,
        metadata->called_function_count,
        metadata->call_count,
        metadata->recorded_call_count,
        escaped_metrics,
        escaped_key
    );

    const char * params[1] = { (const char*)events_data };
    int param_lengths[1] = { events_size };
    int param_formats[1] = { 1 };

    PGresult * res = PQexecParams(priv->conn, query, 1, NULL, params, param_lengths, param_formats, 0);

    PQfreemem(escaped_key);
    PQfreemem(escaped_metrics);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        PQclear(res);
        return -1;
    }

    PQclear(res);
    return 0;
}

static char * escape_json_string(const char * str)
{
    if (!str) return NULL;

    size_t len = strlen(str);
    char * escaped = malloc(len * 2 + 1);
    if (!escaped) return NULL;

    size_t i, j = 0;
    for (i = 0; i < len; i++) {
        switch (str[i]) {
            case '"':
                escaped[j++] = '\\';
                escaped[j++] = '"';
                break;
            case '\\':
                escaped[j++] = '\\';
                escaped[j++] = '\\';
                break;
            case '\n':
                escaped[j++] = '\\';
                escaped[j++] = 'n';
                break;
            case '\r':
                escaped[j++] = '\\';
                escaped[j++] = 'r';
                break;
            case '\t':
                escaped[j++] = '\\';
                escaped[j++] = 't';
                break;
            default:
                escaped[j++] = str[i];
                break;
        }
    }
    escaped[j] = '\0';

    return escaped;
}

static void parse_enabled_metrics_csv(const char * csv_string, int * enabled_metrics)
{
    size_t i;
    for (i = 0; i < SPX_METRIC_COUNT; i++) {
        enabled_metrics[i] = 0;
    }

    if (!csv_string || strlen(csv_string) == 0) {
        return;
    }

    char buffer[512];
    strncpy(buffer, csv_string, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    char * token = strtok(buffer, ",");
    while (token != NULL) {
        // Skip whitespace
        while (*token == ' ') token++;

        int metric_id = atoi(token);
        if (metric_id >= 0 && metric_id < SPX_METRIC_COUNT) {
            enabled_metrics[metric_id] = 1;
        }

        token = strtok(NULL, ",");
    }
}

#endif /* SPX_STORAGE_POSTGRESQL_ENABLED */