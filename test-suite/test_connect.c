
/*
 * machinarium.
 *
 * Cooperative multitasking engine.
*/

#include <machinarium.h>
#include <machinarium_test.h>

#include <arpa/inet.h>

static void
test_connect_fiber(void *arg)
{
	machine_t machine = arg;

	machine_io_t client = machine_create_io(machine);
	test(client != NULL);

	struct sockaddr_in sa;
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = inet_addr("213.180.204.3");
	sa.sin_port = htons(80);
	int rc;
	rc = machine_connect(client, (struct sockaddr *)&sa, INT_MAX);
	if (rc == -1) {
		printf("connection failed: %s\n", machine_error(client));
	} else {
		machine_close(client);
	}

	machine_free_io(client);
}

static void
test_waiter(void *arg)
{
	machine_t machine = arg;

	int id = machine_create_fiber(machine, test_connect_fiber, machine);
	test(id != -1);

	machine_sleep(machine, 0);

	int rc;
	rc = machine_wait(machine, id);
	test(rc == 0);

	machine_stop(machine);
}

void
test_connect(void)
{
	machine_t machine = machine_create();
	test(machine != NULL);

	int rc;
	rc = machine_create_fiber(machine, test_waiter, machine);
	test(rc != -1);

	machine_start(machine);

	rc = machine_free(machine);
	test(rc != -1);
}
