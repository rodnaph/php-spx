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
#include <time.h>

#include <unistd.h>

#include "spx_reporter_full.h"
#include "spx_php.h"
#include "spx_storage.h"

#define BUFFER_CAPACITY 16384


typedef struct {
    spx_profiler_reporter_t base;

    spx_storage_t * storage;
    int owns_storage;
    spx_storage_metadata_t * metadata;
    spx_storage_event_buffer_t * event_buffer;
    spx_storage_function_table_t * function_table;
} full_reporter_t;

static spx_profiler_reporter_cost_t full_notify(
    spx_profiler_reporter_t * reporter,
    const spx_profiler_event_t * event
);

static void full_destroy(spx_profiler_reporter_t * reporter);
static void flush_buffer(full_reporter_t * reporter, const int * enabled_metrics);
static void finalize(full_reporter_t * reporter, const spx_profiler_event_t * event);





spx_profiler_reporter_t * spx_reporter_full_create_with_storage(spx_storage_t * storage)
{
    full_reporter_t * reporter = malloc(sizeof(*reporter));
    if (!reporter) {
        return NULL;
    }

    reporter->base.notify = full_notify;
    reporter->base.destroy = full_destroy;

    reporter->storage = storage;
    reporter->owns_storage = 0;
    reporter->metadata = NULL;
    reporter->event_buffer = NULL;
    reporter->function_table = NULL;

    reporter->metadata = spx_storage_metadata_create();
    if (!reporter->metadata) {
        goto error;
    }

    reporter->event_buffer = spx_storage_event_buffer_create(BUFFER_CAPACITY);
    if (!reporter->event_buffer) {
        goto error;
    }

    reporter->function_table = spx_storage_function_table_create(1024);
    if (!reporter->function_table) {
        goto error;
    }

    if (reporter->storage && reporter->storage->interface->begin_report) {
        if (reporter->storage->interface->begin_report(reporter->storage, reporter->metadata) != 0) {
            goto error;
        }
    }

    return (spx_profiler_reporter_t *) reporter;

error:
    if (reporter->metadata) {
        spx_storage_metadata_destroy(reporter->metadata);
    }
    if (reporter->event_buffer) {
        spx_storage_event_buffer_destroy(reporter->event_buffer);
    }
    if (reporter->function_table) {
        spx_storage_function_table_destroy(reporter->function_table);
    }
    free(reporter);

    return NULL;
}

void spx_reporter_full_set_custom_metadata_str(
    const spx_profiler_reporter_t * base_reporter,
    const char * custom_metadata_str
) {
    const full_reporter_t * reporter = (const full_reporter_t *) base_reporter;

    reporter->metadata->custom_metadata_str = strdup(custom_metadata_str);
}

const char * spx_reporter_full_get_key(const spx_profiler_reporter_t * base_reporter)
{
    const full_reporter_t * reporter = (const full_reporter_t *) base_reporter;

    return reporter->metadata->key;
}

static spx_profiler_reporter_cost_t full_notify(
    spx_profiler_reporter_t * base_reporter,
    const spx_profiler_event_t * event
) {
    full_reporter_t * reporter = (full_reporter_t *) base_reporter;

    if (event->type == SPX_PROFILER_EVENT_CALL_END) {
        reporter->metadata->call_count++;
    }

    if (event->type != SPX_PROFILER_EVENT_FINALIZE) {
        if (event->type == SPX_PROFILER_EVENT_CALL_END) {
            reporter->metadata->recorded_call_count++;
        }

        spx_storage_event_t storage_event;
        storage_event.function_idx = event->callee->idx;
        storage_event.start = event->type == SPX_PROFILER_EVENT_CALL_START;
        storage_event.metric_values = *event->cum;

        spx_storage_event_buffer_append(reporter->event_buffer, &storage_event);

        if (reporter->event_buffer->size < BUFFER_CAPACITY) {
            return SPX_PROFILER_REPORTER_COST_LIGHT;
        }
    }

    flush_buffer(reporter, event->enabled_metrics);

    if (event->type == SPX_PROFILER_EVENT_FINALIZE) {
        finalize(reporter, event);
    }

    return SPX_PROFILER_REPORTER_COST_HEAVY;
}

static void full_destroy(spx_profiler_reporter_t * base_reporter)
{
    full_reporter_t * reporter = (full_reporter_t *) base_reporter;

    if (reporter->storage && reporter->owns_storage) {
        spx_storage_destroy(reporter->storage);
    }

    if (reporter->metadata) {
        spx_storage_metadata_destroy(reporter->metadata);
    }

    if (reporter->event_buffer) {
        spx_storage_event_buffer_destroy(reporter->event_buffer);
    }

    if (reporter->function_table) {
        spx_storage_function_table_destroy(reporter->function_table);
    }
}

static void flush_buffer(full_reporter_t * reporter, const int * enabled_metrics)
{
    if (reporter->event_buffer->size == 0) {
        return;
    }

    if (reporter->storage && reporter->storage->interface->save_events) {
        reporter->storage->interface->save_events(
            reporter->storage,
            reporter->metadata->key,
            reporter->event_buffer,
            enabled_metrics
        );
    }

    reporter->event_buffer->size = 0;
}

static void finalize(full_reporter_t * reporter, const spx_profiler_event_t * event)
{
    size_t i;
    for (i = 0; i < event->func_table.size; i++) {
        const spx_profiler_func_table_entry_t * entry = &event->func_table.entries[i];

        char full_function_name[512];
        snprintf(
            full_function_name,
            sizeof(full_function_name),
            "%s%s%s",
            entry->function.class_name,
            entry->function.class_name[0] ? "::" : "",
            entry->function.func_name
        );

        spx_storage_function_table_append(reporter->function_table, full_function_name);
    }

    reporter->metadata->peak_memory_usage = spx_php_zend_memory_usage();
    reporter->metadata->wall_time_ms = event->cum->values[SPX_METRIC_WALL_TIME] / 1000;
    reporter->metadata->called_function_count = event->func_table.size;

    SPX_METRIC_FOREACH(i, {
        reporter->metadata->enabled_metrics[i] = event->enabled_metrics[i];
    });

    // Save to storage
    if (reporter->storage && reporter->storage->interface->save_functions) {
        reporter->storage->interface->save_functions(
            reporter->storage,
            reporter->metadata->key,
            reporter->function_table
        );
    }

    if (reporter->storage && reporter->storage->interface->finalize_report) {
        reporter->storage->interface->finalize_report(
            reporter->storage,
            reporter->metadata->key,
            reporter->metadata
        );
    }
}
