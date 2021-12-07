
/*
 * Odyssey.
 *
 * Scalable PostgreSQL connection pooler.
 */

#include <kiwi.h>
#include <machinarium.h>
#include <odyssey.h>

od_storage_watchdog_t *od_storage_watchdog_allocate(od_global_t *global)
{
	od_storage_watchdog_t *watchdog;
	watchdog = malloc(sizeof(od_storage_watchdog_t));
	if (watchdog == NULL) {
		return NULL;
	}
	memset(watchdog, 0, sizeof(od_storage_watchdog_t));
	watchdog->check_retry = 10;
	watchdog->global = global;

	return watchdog;
}

int od_storage_watchdog_free(od_storage_watchdog_t *watchdog)
{
	if (watchdog == NULL) {
		return NOT_OK_RESPONSE;
	}

	if (watchdog->query) {
		free(watchdog->query);
	}

	free(watchdog);
	return OK_RESPONSE;
}

od_rule_storage_t *od_rules_storage_allocate(void)
{
	od_rule_storage_t *storage;
	storage = (od_rule_storage_t *)malloc(sizeof(*storage));
	if (storage == NULL)
		return NULL;
	memset(storage, 0, sizeof(*storage));
	storage->tls_opts = od_tls_opts_alloc();
	if (storage->tls_opts == NULL) {
		return NULL;
	}

	od_list_init(&storage->link);
	return storage;
}

void od_rules_storage_free(od_rule_storage_t *storage)
{
	if (storage->name)
		free(storage->name);
	if (storage->type)
		free(storage->type);
	if (storage->host)
		free(storage->host);

	if (storage->tls_opts) {
		od_tls_opts_free(storage->tls_opts);
	}

	if (storage->watchdog) {
		od_storage_watchdog_free(storage->watchdog);
	}

	od_list_unlink(&storage->link);
	free(storage);
}

od_rule_storage_t *od_rules_storage_copy(od_rule_storage_t *storage)
{
	od_rule_storage_t *copy;
	copy = od_rules_storage_allocate();
	if (copy == NULL)
		return NULL;
	copy->storage_type = storage->storage_type;
	copy->name = strdup(storage->name);
	copy->server_max_routing = storage->server_max_routing;
	if (copy->name == NULL)
		goto error;
	copy->type = strdup(storage->type);
	if (copy->type == NULL)
		goto error;
	if (storage->host) {
		copy->host = strdup(storage->host);
		if (copy->host == NULL)
			goto error;
	}
	copy->port = storage->port;
	copy->tls_opts->tls_mode = storage->tls_opts->tls_mode;
	if (storage->tls_opts->tls) {
		copy->tls_opts->tls = strdup(storage->tls_opts->tls);
		if (copy->tls_opts->tls == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_ca_file) {
		copy->tls_opts->tls_ca_file =
			strdup(storage->tls_opts->tls_ca_file);
		if (copy->tls_opts->tls_ca_file == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_key_file) {
		copy->tls_opts->tls_key_file =
			strdup(storage->tls_opts->tls_key_file);
		if (copy->tls_opts->tls_key_file == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_cert_file) {
		copy->tls_opts->tls_cert_file =
			strdup(storage->tls_opts->tls_cert_file);
		if (copy->tls_opts->tls_cert_file == NULL)
			goto error;
	}
	if (storage->tls_opts->tls_protocols) {
		copy->tls_opts->tls_protocols =
			strdup(storage->tls_opts->tls_protocols);
		if (copy->tls_opts->tls_protocols == NULL)
			goto error;
	}
	return copy;
error:
	od_rules_storage_free(copy);
	return NULL;
}

static inline int
od_storage_watchdog_parse_lag_from_datarow(od_logger_t *logger,
					   machine_msg_t *msg, int *repl_lag)
{
	char *pos = (char *)machine_msg_data(msg) + 1;
	uint32_t pos_size = machine_msg_size(msg) - 1;

	/* size */
	uint32_t size;
	int rc;
	rc = kiwi_read32(&size, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1))
		goto error;
	/* count */
	uint16_t count;
	rc = kiwi_read16(&count, &pos, &pos_size);

	if (kiwi_unlikely(rc == -1))
		goto error;

	if (count != 1)
		goto error;

	/* (not used) */
	uint32_t lag_len;
	rc = kiwi_read32(&lag_len, &pos, &pos_size);
	if (kiwi_unlikely(rc == -1)) {
		goto error;
	}

	*repl_lag = strtol(pos, NULL, 0);

	return OK_RESPONSE;
error:
	return NOT_OK_RESPONSE;
}

static inline int od_router_update_heartbit_cb(od_route_t *route, void **argv)
{
	od_route_lock(route);
	route->last_heartbit = argv[0];
	od_route_unlock(route);
	return 0;
}

void od_storage_watchdog_watch(od_storage_watchdog_t *watchdog)
{
	od_global_t *global = watchdog->global;
	od_router_t *router = global->router;
	od_instance_t *instance = global->instance;

	od_debug(&instance->logger, "watchdog", NULL, NULL,
		 "start lag polling watchdog ");

	/* create internal auth client */
	od_client_t *watchdog_client;
	watchdog_client = od_client_allocate();
	if (watchdog_client == NULL) {
		return NOT_OK_RESPONSE;
	}

	watchdog_client->global = global;
	watchdog_client->type = OD_POOL_CLIENT_INTERNAL;
	od_id_generate(&watchdog_client->id, "a");

	/* set storage user and database */
	kiwi_var_set(&watchdog_client->startup.user, KIWI_VAR_UNDEF,
		     watchdog->route_usr, strlen(watchdog->route_usr) + 1);

	kiwi_var_set(&watchdog_client->startup.database, KIWI_VAR_UNDEF,
		     watchdog->route_db, strlen(watchdog->route_db) + 1);

	machine_msg_t *msg;

	int last_heartbit = 0;
	int rc;

	for (;;) {
		/* route */
		od_router_status_t status;
		status = od_router_route(router, watchdog_client);
		od_debug(&instance->logger, "watchdog", watchdog_client, NULL,
			 "routing to internal wd route status: %s",
			 od_router_status_to_str(status));

		if (status != OD_ROUTER_OK) {
			od_client_free(watchdog_client);
			continue;
		}
		od_rule_t *rule = watchdog_client->rule;

		/* attach */
		status = od_router_attach(router, watchdog_client, false);
		od_debug(&instance->logger, "watchdog", watchdog_client, NULL,
			 "attaching wd client to backend connection status: %s",
			 od_router_status_to_str(status));
		if (status != OD_ROUTER_OK) {
			od_router_unroute(router, watchdog_client);
			od_client_free(watchdog_client);
			continue;
		}
		od_server_t *server;
		server = watchdog_client->server;
		od_debug(&instance->logger, "watchdog", NULL, server,
			 "attached to server %s%.*s", server->id.id_prefix,
			 (int)sizeof(server->id.id), server->id.id);

		/* connect to server, if necessary */
		if (server->io.io == NULL) {
			rc = od_backend_connect(server, "watchdog", NULL, NULL);
			if (rc == -1) {
				od_router_close(router, watchdog_client);
				od_router_unroute(router, watchdog_client);
				od_client_free(watchdog_client);
				continue;
			}
		}

		for (int retry = 0; retry < watchdog->check_retry; ++retry) {
			char *qry = watchdog->query;
			od_debug(
				&instance->logger, "watchdog", NULL, NULL,
				"send heartbit arenda update to routes with value %d",
				last_heartbit);

			od_debug(&instance->logger, "watchdog", NULL, NULL,
				 "sizeof %d", strlen(qry) + 1);

			msg = od_query_do(server, "watchdog", qry, NULL);
			if (msg != NULL) {
				rc = od_storage_watchdog_parse_lag_from_datarow(
					&instance->logger, msg, &last_heartbit);
				machine_msg_free(msg);
			} else {
				rc = NOT_OK_RESPONSE;
			}
			if (rc == OK_RESPONSE) {
				od_debug(
					&instance->logger, "watchdog", NULL,
					NULL,
					"send heartbit arenda update to routes with value %d",
					last_heartbit);
				void *argv[] = { last_heartbit };
				od_router_foreach(router,
						  od_router_update_heartbit_cb,
						  argv);
				break;
			}
			// retry
		}

		/* detach and unroute */
		od_router_detach(router, watchdog_client);
		od_router_unroute(router, watchdog_client);
		//od_client_free(watchdog_client);

		/* 1 second soft interval */
		machine_sleep(1000);
	}
}
