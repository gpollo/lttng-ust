/*
 * ltt-context.c
 *
 * Copyright 2011 (c) - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * LTTng UST trace/channel/event context management.
 *
 * Dual LGPL v2.1/GPL v2 license.
 */

#include <lttng/ust-events.h>
#include <lttng/ust-tracer.h>
#include <helper.h>
#include <string.h>
#include <assert.h>

int lttng_find_context(struct lttng_ctx *ctx, const char *name)
{
	unsigned int i;

	for (i = 0; i < ctx->nr_fields; i++) {
		/* Skip allocated (but non-initialized) contexts */
		if (!ctx->fields[i].event_field.name)
			continue;
		if (!strcmp(ctx->fields[i].event_field.name, name))
			return 1;
	}
	return 0;
}

/*
 * Note: as we append context information, the pointer location may change.
 */
struct lttng_ctx_field *lttng_append_context(struct lttng_ctx **ctx_p)
{
	struct lttng_ctx_field *field;
	struct lttng_ctx *ctx;

	if (!*ctx_p) {
		*ctx_p = zmalloc(sizeof(struct lttng_ctx));
		if (!*ctx_p)
			return NULL;
	}
	ctx = *ctx_p;
	if (ctx->nr_fields + 1 > ctx->allocated_fields) {
		struct lttng_ctx_field *new_fields;

		ctx->allocated_fields = max_t(size_t, 1, 2 * ctx->allocated_fields);
		new_fields = zmalloc(ctx->allocated_fields * sizeof(struct lttng_ctx_field));
		if (!new_fields)
			return NULL;
		if (ctx->fields)
			memcpy(new_fields, ctx->fields, sizeof(*ctx->fields) * ctx->nr_fields);
		free(ctx->fields);
		ctx->fields = new_fields;
	}
	field = &ctx->fields[ctx->nr_fields];
	ctx->nr_fields++;
	return field;
}

/*
 * Remove last context field.
 */
void lttng_remove_context_field(struct lttng_ctx **ctx_p,
				struct lttng_ctx_field *field)
{
	struct lttng_ctx *ctx;

	ctx = *ctx_p;
	ctx->nr_fields--;
	assert(&ctx->fields[ctx->nr_fields] == field);
	memset(&ctx->fields[ctx->nr_fields], 0, sizeof(struct lttng_ctx_field));
}

void lttng_destroy_context(struct lttng_ctx *ctx)
{
	int i;

	if (!ctx)
		return;
	for (i = 0; i < ctx->nr_fields; i++) {
		if (ctx->fields[i].destroy)
			ctx->fields[i].destroy(&ctx->fields[i]);
	}
	free(ctx->fields);
	free(ctx);
}