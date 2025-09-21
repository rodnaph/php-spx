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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "spx_storage.h"
#include "spx_php.h"
#include "spx_output_stream.h"
#include "spx_str_builder.h"

#ifdef linux
#   include <sys/syscall.h>
#endif

spx_storage_metadata_t * spx_storage_metadata_create(void)
{
    spx_storage_metadata_t * metadata = malloc(sizeof(*metadata));
    if (!metadata) {
        return NULL;
    }

    metadata->key = NULL;
    metadata->hostname = NULL;
    metadata->process_pwd = NULL;
    metadata->cli_command_line = NULL;
    metadata->http_request_uri = NULL;
    metadata->http_method = NULL;
    metadata->http_host = NULL;
    metadata->custom_metadata_str = NULL;

    metadata->exec_ts = time(NULL);

    char hostname[256];
    if (0 == gethostname(hostname, sizeof(hostname))) {
        hostname[sizeof(hostname) - 1] = 0;
        metadata->hostname = strdup(hostname);
    } else {
        metadata->hostname = strdup("n/a");
    }

    if (!metadata->hostname) {
        goto error;
    }

    metadata->process_pid = getpid();
#ifdef linux
    metadata->process_tid = syscall(SYS_gettid);
#else
    metadata->process_tid = 0;
#endif

    time_t timer;
    time(&timer);

    char date[32];
    strftime(
        date,
        sizeof(date),
        "%Y%m%d_%H%M%S",
        localtime(&timer)
    );

    char key[512];
    snprintf(
        key,
        sizeof(key),
        "spx-full-%s-%s-%d-%d",
        date,
        metadata->hostname,
        metadata->process_pid,
        rand()
    );

    metadata->key = strdup(key);
    if (!metadata->key) {
        goto error;
    }

    char pwd[8 * 1024];
    metadata->process_pwd = strdup(getcwd(pwd, sizeof(pwd)) ? pwd : "n/a");
    if (!metadata->process_pwd) {
        goto error;
    }

    metadata->cli = spx_php_is_cli_sapi();
    metadata->cli_command_line = spx_php_build_command_line();
    if (!metadata->cli_command_line) {
        metadata->cli_command_line = strdup("n/a");
    }

    if (!metadata->cli_command_line) {
        goto error;
    }

    const char * http_request_uri = spx_php_global_array_get("_SERVER", "REQUEST_URI");
    metadata->http_request_uri = strdup(http_request_uri ? http_request_uri : "n/a");
    if (!metadata->http_request_uri) {
        goto error;
    }

    const char * http_method = spx_php_global_array_get("_SERVER", "REQUEST_METHOD");
    metadata->http_method = strdup(http_method ? http_method : "n/a");
    if (!metadata->http_method) {
        goto error;
    }

    const char * http_host = spx_php_global_array_get("_SERVER", "HTTP_HOST");
    metadata->http_host = strdup(http_host ? http_host : "n/a");
    if (!metadata->http_host) {
        goto error;
    }

    metadata->call_count = 0;
    metadata->recorded_call_count = 0;
    metadata->wall_time_ms = 0;
    metadata->peak_memory_usage = 0;
    metadata->called_function_count = 0;

    size_t i;
    for (i = 0; i < SPX_METRIC_COUNT; i++) {
        metadata->enabled_metrics[i] = 0;
    }

    return metadata;

error:
    spx_storage_metadata_destroy(metadata);
    return NULL;
}

void spx_storage_metadata_destroy(spx_storage_metadata_t * metadata)
{
    if (!metadata) {
        return;
    }

    if (metadata->key) {
        free(metadata->key);
    }

    if (metadata->hostname) {
        free(metadata->hostname);
    }

    if (metadata->process_pwd) {
        free(metadata->process_pwd);
    }

    if (metadata->cli_command_line) {
        free(metadata->cli_command_line);
    }

    if (metadata->http_request_uri) {
        free(metadata->http_request_uri);
    }

    if (metadata->http_method) {
        free(metadata->http_method);
    }

    if (metadata->http_host) {
        free(metadata->http_host);
    }

    if (metadata->custom_metadata_str) {
        free(metadata->custom_metadata_str);
    }

    free(metadata);
}

spx_storage_event_buffer_t * spx_storage_event_buffer_create(size_t capacity)
{
    spx_storage_event_buffer_t * buffer = malloc(sizeof(*buffer));
    if (!buffer) {
        return NULL;
    }

    buffer->size = 0;
    buffer->capacity = capacity;
    buffer->events = malloc(sizeof(spx_storage_event_t) * capacity);
    if (!buffer->events) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

void spx_storage_event_buffer_destroy(spx_storage_event_buffer_t * buffer)
{
    if (!buffer) {
        return;
    }

    if (buffer->events) {
        free(buffer->events);
    }

    free(buffer);
}

int spx_storage_event_buffer_append(spx_storage_event_buffer_t * buffer, const spx_storage_event_t * event)
{
    if (!buffer || !event) {
        return -1;
    }

    if (buffer->size >= buffer->capacity) {
        size_t new_capacity = buffer->capacity * 2;
        spx_storage_event_t * new_events = realloc(buffer->events, sizeof(spx_storage_event_t) * new_capacity);
        if (!new_events) {
            return -1;
        }
        buffer->events = new_events;
        buffer->capacity = new_capacity;
    }

    buffer->events[buffer->size] = *event;
    buffer->size++;

    return 0;
}

spx_storage_function_table_t * spx_storage_function_table_create(size_t capacity)
{
    spx_storage_function_table_t * table = malloc(sizeof(*table));
    if (!table) {
        return NULL;
    }

    table->size = 0;
    table->capacity = capacity;
    table->functions = malloc(sizeof(spx_storage_function_t) * capacity);
    if (!table->functions) {
        free(table);
        return NULL;
    }

    return table;
}

void spx_storage_function_table_destroy(spx_storage_function_table_t * table)
{
    if (!table) {
        return;
    }

    if (table->functions) {
        size_t i;
        for (i = 0; i < table->size; i++) {
            if (table->functions[i].name) {
                free(table->functions[i].name);
            }
        }
        free(table->functions);
    }

    free(table);
}

int spx_storage_function_table_append(spx_storage_function_table_t * table, const char * name)
{
    if (!table || !name) {
        return -1;
    }

    if (table->size >= table->capacity) {
        size_t new_capacity = table->capacity * 2;
        spx_storage_function_t * new_functions = realloc(table->functions, sizeof(spx_storage_function_t) * new_capacity);
        if (!new_functions) {
            return -1;
        }
        table->functions = new_functions;
        table->capacity = new_capacity;
    }

    table->functions[table->size].name = strdup(name);
    if (!table->functions[table->size].name) {
        return -1;
    }

    table->size++;

    return 0;
}

spx_storage_t * spx_storage_create(spx_storage_type_t type, const char * config)
{
    switch (type) {
        case SPX_STORAGE_TYPE_FILESYSTEM:
            return spx_storage_create_filesystem(config);

        default:
            spx_php_log_notice("SPX: Unsupported storage type %d", type);
            return NULL;
    }
}

void spx_storage_destroy(spx_storage_t * storage)
{
    if (!storage) {
        return;
    }

    if (storage->interface && storage->interface->destroy) {
        storage->interface->destroy(storage);
    }

    free(storage);
}

int spx_storage_save_events(
    spx_output_stream_t * output,
    spx_str_builder_t * builder,
    const spx_storage_event_buffer_t * events,
    const int * enabled_metrics
)
{
    if (!output) {
        return -1;
    }

    spx_str_builder_reset(builder);

    size_t i;
    for (i = 0; i < events->size; i++) {
        const spx_storage_event_t * event = &events->events[i];

        spx_str_builder_append_long(builder, event->function_idx);
        spx_str_builder_append_char(builder, ' ');
        spx_str_builder_append_char(builder, event->start ? '1' : '0');

        size_t j;
        for (j = 0; j < SPX_METRIC_COUNT; j++) {
            if (enabled_metrics[j]) {
                spx_str_builder_append_char(builder, ' ');
                spx_str_builder_append_double(builder, event->metric_values.values[j], 4);
            }
        }

        spx_str_builder_append_str(builder, "\n");

        if (spx_str_builder_remaining(builder) < 128) {
            spx_output_stream_print(output, spx_str_builder_str(builder));
            spx_str_builder_reset(builder);
        }
    }

    if (spx_str_builder_size(builder) > 0) {
        spx_output_stream_print(output, spx_str_builder_str(builder));
    }

    return 0;
}

int spx_storage_save_functions(
    spx_output_stream_t * output,
    const spx_storage_function_table_t * functions
)
{
    if (!output) {
        return -1;
    }

    spx_output_stream_print(output, "[functions]\n");

    size_t i;
    for (i = 0; i < functions->size; i++) {
        spx_output_stream_printf(
            output,
            "%s\n",
            functions->functions[i].name
        );
    }

    return 0;
}