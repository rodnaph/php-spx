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

#ifndef SPX_STORAGE_H_DEFINED
#define SPX_STORAGE_H_DEFINED

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include <stddef.h>
#include <time.h>
#include <unistd.h>

#include "spx_profiler.h"
#include "spx_output_stream.h"
#include "spx_str_builder.h"

typedef enum {
    SPX_STORAGE_TYPE_FILESYSTEM,
    SPX_STORAGE_TYPE_POSTGRESQL,
} spx_storage_type_t;

typedef struct {
    char * key;
    size_t exec_ts;
    char * hostname;
    pid_t process_pid;
    pid_t process_tid;
    char * process_pwd;
    int cli;
    char * cli_command_line;
    char * http_request_uri;
    char * http_method;
    char * http_host;
    char * custom_metadata_str;
    size_t wall_time_ms;
    size_t peak_memory_usage;
    size_t called_function_count;
    size_t call_count;
    size_t recorded_call_count;
    int enabled_metrics[SPX_METRIC_COUNT];
} spx_storage_metadata_t;

typedef struct {
    size_t function_idx;
    int start;
    spx_profiler_metric_values_t metric_values;
} spx_storage_event_t;

typedef struct {
    size_t size;
    size_t capacity;
    spx_storage_event_t * events;
} spx_storage_event_buffer_t;

typedef struct {
    char * name;
} spx_storage_function_t;

typedef struct {
    size_t size;
    size_t capacity;
    spx_storage_function_t * functions;
} spx_storage_function_table_t;

typedef struct spx_storage_t spx_storage_t;

typedef struct {
    int (*init)(spx_storage_t * storage);
    int (*destroy)(spx_storage_t * storage);

    int (*begin_report)(spx_storage_t * storage, const spx_storage_metadata_t * metadata);
    int (*save_events)(spx_storage_t * storage, const char * key, const spx_storage_event_buffer_t * events, const int * enabled_metrics);
    int (*save_functions)(spx_storage_t * storage, const char * key, const spx_storage_function_table_t * functions);
    int (*finalize_report)(spx_storage_t * storage, const char * key, const spx_storage_metadata_t * metadata);

    size_t (*list_reports)(spx_storage_t * storage, void (*callback)(const char * metadata_json, size_t count));
    int (*get_report_metadata)(spx_storage_t * storage, const char * key, char ** metadata_json);
    int (*get_report_events)(spx_storage_t * storage, const char * key, char ** events_data, size_t * size);
} spx_storage_interface_t;

struct spx_storage_t {
    spx_storage_type_t type;
    const spx_storage_interface_t * interface;
    void * private_data;
};

spx_storage_metadata_t * spx_storage_metadata_create(void);
void spx_storage_metadata_destroy(spx_storage_metadata_t * metadata);

spx_storage_event_buffer_t * spx_storage_event_buffer_create(size_t capacity);
void spx_storage_event_buffer_destroy(spx_storage_event_buffer_t * buffer);
int spx_storage_event_buffer_append(spx_storage_event_buffer_t * buffer, const spx_storage_event_t * event);

spx_storage_function_table_t * spx_storage_function_table_create(size_t capacity);
void spx_storage_function_table_destroy(spx_storage_function_table_t * table);
int spx_storage_function_table_append(spx_storage_function_table_t * table, const char * name);

spx_storage_t * spx_storage_create(spx_storage_type_t type, const char * config);
void spx_storage_destroy(spx_storage_t * storage);

spx_storage_t * spx_storage_create_filesystem(const char * data_dir);
#ifdef SPX_STORAGE_POSTGRESQL_ENABLED
spx_storage_t * spx_storage_create_postgresql(const char * connection_string);
#endif

int spx_storage_save_events(
    spx_output_stream_t * output,
    spx_str_builder_t * builder,
    const spx_storage_event_buffer_t * events,
    const int * enabled_metrics
);

int spx_storage_save_functions(
    spx_output_stream_t * output,
    const spx_storage_function_table_t * functions
);

#endif /* SPX_STORAGE_H_DEFINED */