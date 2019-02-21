/*
 * Copyright (c) 2019 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef _LTTNG_UST_MALLOC_CONFIG_H
#define _LTTNG_UST_MALLOC_CONFIG_H

/*
 * This file contains the configuration for using dlmalloc in LTTng-UST.
 */

/* Only use mmap, not sbrk, leaving sbrk for application malloc. */
#define HAVE_MORECORE	0
#define USE_DL_PREFIX

/* Redefine all function names using our prefix. */
#define dlcalloc                     lttng_ust_calloc
#define dlfree                       lttng_ust_free
#define dlmalloc                     lttng_ust_malloc
#define dlmemalign                   lttng_ust_memalign
#define dlposix_memalign             lttng_ust_posix_memalign
#define dlrealloc                    lttng_ust_realloc
#define dlrealloc_in_place           lttng_ust_realloc_in_place
#define dlvalloc                     lttng_ust_valloc
#define dlpvalloc                    lttng_ust_pvalloc
#define dlmallinfo                   lttng_ust_mallinfo
#define dlmallopt                    lttng_ust_mallopt
#define dlmalloc_trim                lttng_ust_malloc_trim
#define dlmalloc_stats               lttng_ust_malloc_stats
#define dlmalloc_usable_size         lttng_ust_malloc_usable_size
#define dlmalloc_footprint           lttng_ust_malloc_footprint
#define dlmalloc_max_footprint       lttng_ust_malloc_max_footprint
#define dlmalloc_footprint_limit     lttng_ust_malloc_footprint_limit
#define dlmalloc_set_footprint_limit lttng_ust_malloc_set_footprint_limit
#define dlmalloc_inspect_all         lttng_ust_malloc_inspect_all
#define dlindependent_calloc         lttng_ust_independent_calloc
#define dlindependent_comalloc       lttng_ust_independent_comalloc
#define dlbulk_free                  lttng_ust_bulk_free

#endif /* _LTTNG_UST_MALLOC_CONFIG_H */
