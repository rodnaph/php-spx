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


#ifndef SPX_REPORTER_FULL_H_DEFINED
#define SPX_REPORTER_FULL_H_DEFINED

#include "spx_profiler.h"

struct spx_storage_t;

spx_profiler_reporter_t * spx_reporter_full_create_with_storage(struct spx_storage_t * storage);

void spx_reporter_full_set_custom_metadata_str(
    const spx_profiler_reporter_t * base_reporter,
    const char * custom_metadata_str
);

const char * spx_reporter_full_get_key(const spx_profiler_reporter_t * base_reporter);

#endif /* SPX_REPORTER_FULL_H_DEFINED */
