#include <dlfcn.h>

#include "snap.h"


struct g_snap_ctx {
	pthread_mutex_t			lock;
	TAILQ_HEAD(, snap_driver)	drivers_list;
};

static struct g_snap_ctx g_sctx;

void snap_unregister_driver(struct snap_driver *driver)
{
	struct snap_driver *tmp, *next;

	pthread_mutex_lock(&g_sctx.lock);
	TAILQ_FOREACH_SAFE(tmp, &g_sctx.drivers_list, entry, next) {
		if (tmp == driver) {
			TAILQ_REMOVE(&g_sctx.drivers_list, driver, entry);
			break;
		}
	}
	pthread_mutex_unlock(&g_sctx.lock);
}

void snap_register_driver(struct snap_driver *driver)
{
	struct snap_driver *tmp;
	bool found = false;

	pthread_mutex_lock(&g_sctx.lock);
	TAILQ_FOREACH(tmp, &g_sctx.drivers_list, entry) {
		if (tmp == driver) {
			found = true;
			break;
		}
	}

	if (!found)
		TAILQ_INSERT_HEAD(&g_sctx.drivers_list, driver, entry);
	pthread_mutex_unlock(&g_sctx.lock);
}

bool snap_is_capable_device(struct ibv_device *ibdev)
{
	bool found = false;
	struct snap_driver *driver;

	pthread_mutex_lock(&g_sctx.lock);
	TAILQ_FOREACH(driver, &g_sctx.drivers_list, entry) {
		if (!strncmp(driver->name, ibdev->name,
		    strlen(driver->name))) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_sctx.lock);

	if (found)
		return driver->is_capable(ibdev);

	return false;

}

struct snap_context *snap_create_context(struct ibv_device *ibdev)
{
	bool found = false;
	struct snap_driver *driver;
	struct snap_context *sctx;
	int rc;

	pthread_mutex_lock(&g_sctx.lock);
	TAILQ_FOREACH(driver, &g_sctx.drivers_list, entry) {
		if (!strncmp(driver->name, ibdev->name,
		    strlen(driver->name))) {
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&g_sctx.lock);

	if (!found)
		return NULL;

	sctx = driver->create(ibdev);
	if (!sctx)
		return NULL;
	else
		sctx->driver = driver;

	return sctx;
}

void snap_destroy_context(struct snap_context *sctx)
{
	sctx->driver->destroy(sctx);
}

struct snap_device *snap_open_device(struct snap_context *sctx,
				     struct snap_device_attr *attr)
{
	struct snap_driver *driver = sctx->driver;
	struct snap_device *sdev;

	sdev = driver->open(sctx, attr);
	if (!sdev)
		return NULL;
	else
		sdev->sctx = sctx;

	return sdev;
}

void snap_close_device(struct snap_device *sdev)
{
	struct snap_driver *driver = sdev->sctx->driver;

	driver->close(sdev);
}

int snap_open()
{
	bool found = false;
	struct snap_driver *driver;
	void *dlhandle;
	int rc;

	rc = pthread_mutex_init(&g_sctx.lock, NULL);
	if (rc)
		goto out_err;

	TAILQ_INIT(&g_sctx.drivers_list);

	dlhandle = dlopen("libmlx5_snap.so", RTLD_LAZY);
	if (!dlhandle) {
		fprintf(stderr, PFX "couldn't load mlx5 driver.\n");
		goto out_mutex_destroy;
	}

	TAILQ_FOREACH(driver, &g_sctx.drivers_list, entry) {
		if (!strcmp(driver->name, "mlx5")) {
			driver->dlhandle = dlhandle;
			found = true;
			break;
		}
	}

	if (!found)
		goto out_close;

	return 0;

out_close:
	dlclose(dlhandle);
out_mutex_destroy:
	pthread_mutex_destroy(&g_sctx.lock);
out_err:
	return rc;
}

void snap_close()
{
	struct snap_driver *driver, *next;

	TAILQ_FOREACH_SAFE(driver, &g_sctx.drivers_list, entry, next)
		dlclose(driver->dlhandle);

	pthread_mutex_destroy(&g_sctx.lock);
}
