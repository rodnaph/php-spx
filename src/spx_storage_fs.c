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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "spx_storage.h"
#include "spx_output_stream.h"
#include "spx_str_builder.h"
#include "spx_utils.h"
#include "spx_metric.h"

#define BUFFER_CAPACITY 16384

typedef struct {
    char * data_dir;
    spx_output_stream_t * current_output;
    spx_str_builder_t * str_builder;
} spx_storage_fs_private_t;

static int fs_init(spx_storage_t * storage);
static int fs_destroy(spx_storage_t * storage);
static int fs_begin_report(spx_storage_t * storage, const spx_storage_metadata_t * metadata);
static int fs_save_events(spx_storage_t * storage, const char * key, const spx_storage_event_buffer_t * events, const int * enabled_metrics);
static int fs_save_functions(spx_storage_t * storage, const char * key, const spx_storage_function_table_t * functions);
static int fs_finalize_report(spx_storage_t * storage, const char * key, const spx_storage_metadata_t * metadata);
static size_t fs_list_reports(spx_storage_t * storage, void (*callback)(const char * metadata_json, size_t count));
static int fs_get_report_metadata(spx_storage_t * storage, const char * key, char ** metadata_json);
static int fs_get_report_events(spx_storage_t * storage, const char * key, char ** events_data, size_t * size);

static int build_metadata_file_name(const char * data_dir, const char * key, char * file_name, size_t size);
static int build_events_file_name(const char * data_dir, const char * key, char * file_name, size_t size);
static int save_metadata_to_file(const spx_storage_metadata_t * metadata, const char * file_name);

static const spx_storage_interface_t fs_interface = {
    .init = fs_init,
    .destroy = fs_destroy,
    .begin_report = fs_begin_report,
    .save_events = fs_save_events,
    .save_functions = fs_save_functions,
    .finalize_report = fs_finalize_report,
    .list_reports = fs_list_reports,
    .get_report_metadata = fs_get_report_metadata,
    .get_report_events = fs_get_report_events,
};

spx_storage_t * spx_storage_create_filesystem(const char * data_dir)
{
    spx_storage_t * storage = malloc(sizeof(*storage));
    if (!storage) {
        return NULL;
    }

    spx_storage_fs_private_t * priv = malloc(sizeof(*priv));
    if (!priv) {
        free(storage);
        return NULL;
    }

    priv->data_dir = strdup(data_dir);
    if (!priv->data_dir) {
        free(priv);
        free(storage);
        return NULL;
    }

    priv->current_output = NULL;
    priv->str_builder = NULL;

    storage->type = SPX_STORAGE_TYPE_FILESYSTEM;
    storage->interface = &fs_interface;
    storage->private_data = priv;

    if (storage->interface->init && storage->interface->init(storage) != 0) {
        spx_storage_destroy(storage);
        return NULL;
    }

    return storage;
}

static int fs_init(spx_storage_t * storage)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    priv->str_builder = spx_str_builder_create(8 * 1024);
    if (!priv->str_builder) {
        return -1;
    }

    (void) mkdir(priv->data_dir, 0777);

    return 0;
}

static int fs_destroy(spx_storage_t * storage)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    if (priv->current_output) {
        spx_output_stream_close(priv->current_output);
    }

    if (priv->str_builder) {
        spx_str_builder_destroy(priv->str_builder);
    }

    if (priv->data_dir) {
        free(priv->data_dir);
    }

    free(priv);

    return 0;
}

static int fs_begin_report(spx_storage_t * storage, const spx_storage_metadata_t * metadata)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    char file_name[PATH_MAX];
    if (build_events_file_name(priv->data_dir, metadata->key, file_name, sizeof(file_name)) != 0) {
        return -1;
    }

    priv->current_output = spx_output_stream_open(file_name, 1);
    if (!priv->current_output) {
        return -1;
    }

    spx_output_stream_print(priv->current_output, "[events]\n");

    return 0;
}

static int fs_save_events(spx_storage_t * storage, const char * key, const spx_storage_event_buffer_t * events, const int * enabled_metrics)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;
    return spx_storage_save_events(priv->current_output, priv->str_builder, events, enabled_metrics);
}

static int fs_save_functions(spx_storage_t * storage, const char * key, const spx_storage_function_table_t * functions)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;
    return spx_storage_save_functions(priv->current_output, functions);
}

static int fs_finalize_report(spx_storage_t * storage, const char * key, const spx_storage_metadata_t * metadata)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    if (priv->current_output) {
        spx_output_stream_close(priv->current_output);
        priv->current_output = NULL;
    }

    char metadata_file_name[PATH_MAX];
    if (build_metadata_file_name(priv->data_dir, key, metadata_file_name, sizeof(metadata_file_name)) != 0) {
        return -1;
    }

    return save_metadata_to_file(metadata, metadata_file_name);
}

static size_t fs_list_reports(spx_storage_t * storage, void (*callback)(const char * metadata_json, size_t count))
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    DIR * dir = opendir(priv->data_dir);
    if (!dir) {
        return 0;
    }

    size_t count = 0;
    const struct dirent * entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!spx_utils_str_ends_with(entry->d_name, ".json")) {
            continue;
        }

        char file_path[PATH_MAX];
        snprintf(file_path, sizeof(file_path), "%s/%s", priv->data_dir, entry->d_name);

        FILE * fp = fopen(file_path, "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long file_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);

            char * metadata_json = malloc(file_size + 1);
            if (metadata_json) {
                size_t read_bytes = fread(metadata_json, 1, file_size, fp);
                metadata_json[read_bytes] = '\0';
                callback(metadata_json, count);
                free(metadata_json);
                count++;
            }
            fclose(fp);
        }
    }

    closedir(dir);
    return count;
}

static int fs_get_report_metadata(spx_storage_t * storage, const char * key, char ** metadata_json)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    char file_name[PATH_MAX];
    if (build_metadata_file_name(priv->data_dir, key, file_name, sizeof(file_name)) != 0) {
        return -1;
    }

    FILE * fp = fopen(file_name, "r");
    if (!fp) {
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    *metadata_json = malloc(file_size + 1);
    if (!*metadata_json) {
        fclose(fp);
        return -1;
    }

    size_t read_bytes = fread(*metadata_json, 1, file_size, fp);
    (*metadata_json)[read_bytes] = '\0';
    fclose(fp);

    return 0;
}

static int fs_get_report_events(spx_storage_t * storage, const char * key, char ** events_data, size_t * size)
{
    spx_storage_fs_private_t * priv = (spx_storage_fs_private_t *) storage->private_data;

    char file_name[PATH_MAX];
    if (build_events_file_name(priv->data_dir, key, file_name, sizeof(file_name)) != 0) {
        return -1;
    }

    FILE * fp = fopen(file_name, "rb");
    if (!fp) {
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    *events_data = malloc(file_size);
    if (!*events_data) {
        fclose(fp);
        return -1;
    }

    *size = fread(*events_data, 1, file_size, fp);
    fclose(fp);

    return 0;
}


static int build_metadata_file_name(const char * data_dir, const char * key, char * file_name, size_t size)
{
    int ret = snprintf(file_name, size, "%s/%s.json", data_dir, key);
    return (ret < 0 || ret >= size) ? -1 : 0;
}

static int build_events_file_name(const char * data_dir, const char * key, char * file_name, size_t size)
{
    int ret = snprintf(file_name, size, "%s/%s.txt.gz", data_dir, key);
    return (ret < 0 || ret >= size) ? -1 : 0;
}

static int save_metadata_to_file(const spx_storage_metadata_t * metadata, const char * file_name)
{
    FILE * fp = fopen(file_name, "w");
    if (!fp) {
        return -1;
    }

    char buf[8 * 1024];

    fprintf(fp, "{\n");

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "key",
        spx_utils_json_escape(buf, metadata->key, sizeof(buf))
    );

    fprintf(
        fp,
        "  \"%s\": %zu,\n",
        "exec_ts",
        metadata->exec_ts
    );

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "host_name",
        spx_utils_json_escape(buf, metadata->hostname, sizeof(buf))
    );

    fprintf(
        fp,
        "  \"%s\": %d,\n",
        "process_pid",
        metadata->process_pid
    );

    fprintf(
        fp,
        "  \"%s\": %d,\n",
        "process_tid",
        metadata->process_tid
    );

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "process_pwd",
        spx_utils_json_escape(buf, metadata->process_pwd, sizeof(buf))
    );

    fprintf(
        fp,
        "  \"%s\": %d,\n",
        "cli",
        metadata->cli
    );

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "cli_command_line",
        spx_utils_json_escape(buf, metadata->cli_command_line, sizeof(buf))
    );

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "http_request_uri",
        spx_utils_json_escape(buf, metadata->http_request_uri, sizeof(buf))
    );

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "http_method",
        spx_utils_json_escape(buf, metadata->http_method, sizeof(buf))
    );

    fprintf(
        fp,
        "  \"%s\": \"%s\",\n",
        "http_host",
        spx_utils_json_escape(buf, metadata->http_host, sizeof(buf))
    );

    if (metadata->custom_metadata_str) {
        fprintf(
            fp,
            "  \"%s\": \"%s\",\n",
            "custom_metadata_str",
            spx_utils_json_escape(buf, metadata->custom_metadata_str, sizeof(buf))
        );
    } else {
        fprintf(
            fp,
            "  \"%s\": null,\n",
            "custom_metadata_str"
        );
    }

    fprintf(
        fp,
        "  \"%s\": %zu,\n",
        "wall_time_ms",
        metadata->wall_time_ms
    );

    fprintf(
        fp,
        "  \"%s\": %zu,\n",
        "peak_memory_usage",
        metadata->peak_memory_usage
    );

    fprintf(
        fp,
        "  \"%s\": %zu,\n",
        "called_function_count",
        metadata->called_function_count
    );

    fprintf(
        fp,
        "  \"%s\": %zu,\n",
        "call_count",
        metadata->call_count
    );

    fprintf(
        fp,
        "  \"%s\": %zu,\n",
        "recorded_call_count",
        metadata->recorded_call_count
    );

    fprintf(fp, "  \"enabled_metrics\": [\n");

    int first = 1;
    size_t i;
    for (i = 0; i < SPX_METRIC_COUNT; i++) {
        if (!metadata->enabled_metrics[i]) {
            continue;
        }

        fprintf(fp, "    ");

        if (!first) {
            fprintf(fp, ",");
        }

        fprintf(fp, "\"%s\"\n", spx_metric_info[i].key);

        first = 0;
    }

    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    fclose(fp);

    return 0;
}