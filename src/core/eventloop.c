#include <aura/aura.h>
#include <aura/private.h>
#include <aura/eventloop.h>

/** \addtogroup loop
 *  @{
 */

static void eventloop_fd_changed_cb(const struct aura_pollfds *fd, enum aura_fd_action act, void *arg)
{
	struct aura_eventloop *loop = arg;
	loop->module->fd_action(loop, fd, act);
}

/**
 * Add a node to existing event loop.
 *
 * WARNING: This node should not be registered in any other manually created event loops or a panic
 * will occur.
 *
 * @param loop
 * @param node
 */
void aura_eventloop_add(struct aura_eventloop *loop, struct aura_node *node)
{
	struct aura_eventloop *curloop = aura_node_eventloop_get(node);
	struct aura_timer *pos;

	/* Some sanity checking first */
	if ((curloop != NULL) && (!node->evtloop_is_autocreated))
		BUG(node, "Specified node is already bound to an event-system");

	if (curloop != NULL) {
		slog(4, SLOG_DEBUG, "eventloop: Node has an associated auto-created eventsystem, destroying...");
		aura_eventloop_destroy(curloop);
	}

	/* Link our next node into our list and adjust timeouts */
	list_add_tail(&node->eventloop_node_list, &loop->nodelist);
	aura_node_eventloop_set(node, loop);

	loop->module->node_added(loop, node);

	/* Set up our fdaction callback to handle descriptor changes */
	aura_fd_changed_cb(node, eventloop_fd_changed_cb, loop);

	/* Start all node's timers, if any */
	list_for_each_entry(pos, &node->timer_list, entry) {
		if (pos->is_active) {
			pos->is_active = false;
			aura_timer_start(pos, pos->flags, NULL);
		}
	}
}

/**
 * Remove a node from it's associated event loop.
 *
 * WARNING: If the node is not bound to any event loop a panic will occur
 * @param loop
 * @param node
 */
void aura_eventloop_del(struct aura_node *node)
{
	const struct aura_pollfds *fds;
	int i, count;
	struct aura_eventloop *loop = aura_node_eventloop_get(node);
	struct aura_timer *pos;

	/* Some sanity checking first */
	if (loop == NULL)
		BUG(node, "Specified node is not bound to any eventloop");

	/* Stop all running timers, but keep their 'running' flag */
	list_for_each_entry(pos, &node->timer_list, entry) {
		if (pos->is_active) {
			aura_timer_stop(pos);
			pos->is_active = true;
		}
	}

	loop->module->node_removed(loop, node);
	/* Remove our node from the list */
	list_del(&node->eventloop_node_list);
	aura_node_eventloop_set(node, NULL);

	/* Remove all descriptors from epoll, but keep 'em in the node */
	count = aura_get_pollfds(node, &fds);
	for (i = 0; i < count; i++)
		loop->module->fd_action(loop, &fds[i], AURA_FD_REMOVED);

	/* Remove our fd_changed callback */
	aura_fd_changed_cb(node, NULL, NULL);
	node->evtloop_is_autocreated = 0;
}


/**
 * Create an empty eventloop with no nodes
 *
 * @return Pointer to eventloop object or NULL
 */
void *aura_eventloop_create_empty()
{
	struct aura_eventloop *loop = calloc(1, sizeof(*loop));

	if (!loop)
		return NULL;

	INIT_LIST_HEAD(&loop->nodelist);
	loop->poll_timeout = 5000;
	loop->module = aura_eventloop_module_get();

	if (!loop->module)
		BUG(NULL, "Internal BUG - no eventloop module selected!");

	if (0 != loop->module->create(loop))
		goto err_free_loop;

	/* Just return the loop, we're good */
	return loop;

err_free_loop:
	free(loop);
	return NULL;
}

/**
 * Create an event loop from a NULL-terminated list of nodes passed in va_list
 *
 * @param ap
 */
void *aura_eventloop_vcreate(va_list ap)
{
	struct aura_node *node;

	struct aura_eventloop *loop = aura_eventloop_create_empty();

	if (!loop)
		return NULL;

	/* Add all our nodes to this loop */
	while ((node = va_arg(ap, struct aura_node *)))
		aura_eventloop_add(loop, node);

	/* Return the loop, we're good */
	return loop;
}

/**
 * Create an event loop from a list of null-terminated nodes.
 * Do not use this function. See aura_eventloop_create() macro
 * @param dummy a dummy parameter required by va_start
 * @param ... A NULL-terminated list of nodes
 */
void *aura_eventloop_create__(int dummy, ...)
{
	void *ret;
	va_list ap;

	va_start(ap, dummy);
	ret = aura_eventloop_vcreate(ap);
	va_end(ap);
	return ret;
}

/**
 * Destroy and eventloop and deassociate any nodes from it.
 * This function does NOT close any open nodes. You have to
 * do it yourself
 *
 * @param loop
 */
void aura_eventloop_destroy(struct aura_eventloop *loop)
{
	struct list_head *pos;
	struct list_head *tmp;

	list_for_each_safe(pos, tmp, &loop->nodelist) {
		struct aura_node *node = list_entry(pos, struct aura_node,
						    eventloop_node_list);

		aura_eventloop_del(node);
	}

	loop->module->destroy(loop);
	free(loop);
}

/**
 * Handle events in the specified loop forever or
 * until someone calls aura_eventloop_break()
 *
 * @param loop
 */
void aura_eventloop_dispatch(struct aura_eventloop *loop, int flags)
{
	struct aura_node *node;
	list_for_each_entry(node, &loop->nodelist, eventloop_node_list)
	{
		if (!node->start_event_sent) {
			node->start_event_sent = true;
			aura_node_dispatch_event(node, NODE_EVENT_STARTED, NULL);
			/* If we're waiting for a specific status - return now!
				The node may go online handling the 'started' event
			*/
			if (node->waiting_for_status)
				return;
		}
	}
	loop->module->dispatch(loop, flags);
}

void aura_eventloop_loopexit(struct aura_eventloop *loop, struct timeval *tv)
{
	loop->module->loopbreak(loop, tv);
}

/**
 * @}
 */
