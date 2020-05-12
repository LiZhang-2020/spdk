#include "snap_virtio_net.h"

#include "mlx5_ifc.h"

/**
 * snap_virtio_net_query_device() - Query an Virtio net snap device
 * @sdev:       snap device
 * @attr:       Virtio net snap device attr container (output)
 *
 * Query a Virtio net snap device. Attr argument must have enough space for
 * the output data.
 *
 * Return: Returns 0 in case of success and attr is filled.
 */
int snap_virtio_net_query_device(struct snap_device *sdev,
	struct snap_virtio_net_device_attr *attr)
{
	uint8_t *out;
	struct snap_context *sctx = sdev->sctx;
	uint8_t *device_emulation_out;
	int i, ret, out_size;
	uint64_t dev_allowed;

	if (attr->queues > sctx->virtio_net_caps.max_emulated_virtqs)
		return -EINVAL;

	out_size = DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr) +
		   DEVX_ST_SZ_BYTES(virtio_net_device_emulation) +
		   attr->queues * DEVX_ST_SZ_BYTES(virtio_q_layout);
	out = calloc(1, out_size);
	if (!out)
		return -ENOMEM;

	ret = snap_virtio_query_device(sdev, SNAP_VIRTIO_NET, out, out_size);
	if (ret)
		goto out_free;

	device_emulation_out = out + DEVX_ST_SZ_BYTES(general_obj_out_cmd_hdr);

	snap_virtio_get_device_attr(sdev, &attr->vattr,
				    DEVX_ADDR_OF(virtio_net_device_emulation,
						 device_emulation_out,
						 virtio_device));
	if (attr->queues) {
		for (i = 0; i < attr->queues; i++)
			snap_virtio_get_queue_attr(&attr->q_attrs[i].vattr,
						   DEVX_ADDR_OF(virtio_net_device_emulation,
								device_emulation_out,
								virtio_q_configuration[i]));
	}

	attr->vattr.enabled = DEVX_GET(virtio_net_device_emulation,
				       device_emulation_out, enabled);
	attr->modifiable_fields = 0;
	dev_allowed = DEVX_GET64(virtio_net_device_emulation,
				 device_emulation_out, modify_field_select);
	if (dev_allowed) {
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_STATUS)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_DEV_STATUS;
		if (dev_allowed & MLX5_VIRTIO_DEVICE_MODIFY_ENABLED)
			attr->modifiable_fields |= SNAP_VIRTIO_MOD_ENABLED;
	}
	attr->mtu = DEVX_GET(virtio_net_device_emulation,
			     device_emulation_out, virtio_net_config.mtu);
	attr->status = DEVX_GET(virtio_net_device_emulation,
				device_emulation_out, virtio_net_config.status);
	attr->max_queue_pairs = DEVX_GET(virtio_net_device_emulation,
					 device_emulation_out, virtio_net_config.max_virtqueue_pairs);
	attr->mac = (uint64_t)DEVX_GET(virtio_net_device_emulation,
				       device_emulation_out, virtio_net_config.mac_47_16) << 16;
	attr->mac |= DEVX_GET(virtio_net_device_emulation,
			      device_emulation_out, virtio_net_config.mac_15_0);

out_free:
	free(out);
	return ret;
}

static int
snap_virtio_net_get_modifiable_device_fields(struct snap_device *sdev)
{
	struct snap_virtio_net_device_attr attr = {};
	int ret;

	ret = snap_virtio_net_query_device(sdev, &attr);
	if (ret)
		return ret;

	sdev->mod_allowed_mask = attr.modifiable_fields;

	return 0;
}

/**
 * snap_virtio_net_modify_device() - Modify Virtio net snap device
 * @sdev:       snap device
 * @mask:       selected params to modify (mask of enum snap_virtio_dev_modify)
 * @attr:       attributes for the net device modify
 *
 * Modify Virtio net snap device object according to a given mask.
 *
 * Return: 0 on success.
 */
int snap_virtio_net_modify_device(struct snap_device *sdev, uint64_t mask,
		struct snap_virtio_net_device_attr *attr)
{
	int ret;

	if (!sdev->mod_allowed_mask) {
		ret = snap_virtio_net_get_modifiable_device_fields(sdev);
		if (ret)
			return ret;
	}

	return snap_virtio_modify_device(sdev, SNAP_VIRTIO_NET, mask,
					 &attr->vattr);
}

/**
 * snap_virtio_net_init_device() - Initialize a new snap device with VIRTIO
 *                                 net characteristics
 * @sdev:       snap device
 *
 * Initialize a snap device for Virtio net emulation. Allocate the needed
 * resources in the HCA and setup internal context.
 *
 * Return: Returns 0 in case of success.
 */
int snap_virtio_net_init_device(struct snap_device *sdev)
{
	struct snap_virtio_net_device *vndev;
	int ret, i;

	if (sdev->pci->type != SNAP_VIRTIO_NET_PF &&
	    sdev->pci->type != SNAP_VIRTIO_NET_VF)
		return -EINVAL;

	vndev = calloc(1, sizeof(*vndev));
	if (!vndev)
		return -ENOMEM;

	vndev->vdev.num_queues = sdev->sctx->virtio_net_caps.max_emulated_virtqs;

	vndev->virtqs = calloc(vndev->vdev.num_queues, sizeof(*vndev->virtqs));
	if (!vndev->virtqs) {
		ret = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < vndev->vdev.num_queues; i++)
		vndev->virtqs[i].vndev = vndev;

	ret = snap_init_device(sdev);
	if (ret)
		goto out_free_virtqs;

	sdev->dd_data = vndev;
	vndev->vdev.sdev = sdev;

	return 0;

out_free_virtqs:
	free(vndev->virtqs);
out_free:
	free(vndev);
	return ret;
}

static int snap_consume_virtio_net_queue_event(struct mlx5_snap_devx_obj *obj,
		struct snap_event *sevent)
{
	struct snap_device *sdev = obj->sdev;
	struct snap_virtio_net_device *vndev;
	struct snap_virtio_net_queue *vnq = NULL;
	int i;

	if (sdev->pci->type != SNAP_VIRTIO_NET_PF &&
	    sdev->pci->type != SNAP_VIRTIO_NET_VF)
		return -EINVAL;

	vndev = (struct snap_virtio_net_device *)sdev->dd_data;
	for (i = 0; i < vndev->vdev.num_queues; i++) {
		if (vndev->virtqs[i].virtq.virtq == obj) {
			vnq = &vndev->virtqs[i];
			break;
		}
	}

	if (!vnq)
		return -EINVAL;

	sevent->type = SNAP_EVENT_VIRTIO_NET_QUEUE_CHANGE;
	sevent->obj = vnq;

	return 0;
}

/**
 * snap_virtio_net_teardown_device() - Teardown Virtio net specifics from a
 *                                     snap device
 * @sdev:       snap device
 *
 * Teardown and free Virtio net context from a snap device.
 *
 * Return: Returns 0 in case of success.
 */
int snap_virtio_net_teardown_device(struct snap_device *sdev)
{
	struct snap_virtio_net_device *vndev;
	int ret = 0;

	vndev = (struct snap_virtio_net_device *)sdev->dd_data;
	if (sdev->pci->type != SNAP_VIRTIO_NET_PF &&
	    sdev->pci->type != SNAP_VIRTIO_NET_VF)
		return -EINVAL;

	sdev->dd_data = NULL;

	ret = snap_teardown_device(sdev);

	free(vndev->virtqs);
	free(vndev);

	return ret;
}

/**
 * snap_virtio_net_query_queue() - Query a Virtio net queue object
 * @vnq:        snap Virtio net queue
 * @attr:       attributes for the queue query (output)
 *
 * Query a Virtio net snap queue object.
 *
 * Return: 0 on success, and attr is filled with the query result.
 */
int snap_virtio_net_query_queue(struct snap_virtio_net_queue *vnq,
		struct snap_virtio_net_queue_attr *attr)
{
	return snap_virtio_query_queue(&vnq->virtq, &attr->vattr);
}

/**
 * snap_virtio_net_create_queue() - Create a new Virtio net snap queue object
 * @sdev:       snap device
 * @attr:       attributes for the queue creation
 *
 * Create a Virtio net snap queue object with the given attributes.
 *
 * Return: Returns snap_virtio_net_queue in case of success, NULL otherwise and
 * errno will be set to indicate the failure reason.
 */
struct snap_virtio_net_queue*
snap_virtio_net_create_queue(struct snap_device *sdev,
	struct snap_virtio_net_queue_attr *attr)
{
	struct snap_virtio_net_device *vndev;
	struct snap_virtio_net_queue *vnq;
	int ret;

	vndev = (struct snap_virtio_net_device *)sdev->dd_data;

	if (attr->vattr.idx >= vndev->vdev.num_queues) {
		errno = EINVAL;
		goto out;
	}

	vnq = &vndev->virtqs[attr->vattr.idx];

	ret = snap_virtio_init_virtq_umem(sdev->sctx,
					  &sdev->sctx->virtio_net_caps,
					  &vnq->virtq, attr->vattr.size);
	if (ret) {
		errno = ret;
		goto out;
	}

	vnq->virtq.virtq = snap_virtio_create_queue(sdev, &attr->vattr,
						    vnq->virtq.umem);
	if (!vnq->virtq.virtq)
		goto out_umem;

	if (sdev->mdev.channel) {
		uint16_t ev_type = MLX5_EVENT_TYPE_OBJECT_CHANGE;

		ret = mlx5dv_devx_subscribe_devx_event(sdev->mdev.channel,
			vnq->virtq.virtq->obj,
			sizeof(ev_type), &ev_type,
			(uint64_t)vnq->virtq.virtq);
		if (ret)
			goto destroy_queue;

		vnq->virtq.virtq->consume_event = snap_consume_virtio_net_queue_event;
	}

	vnq->virtq.idx = attr->vattr.idx;

	return vnq;

destroy_queue:
	snap_devx_obj_destroy(vnq->virtq.virtq);
out_umem:
	snap_virtio_teardown_virtq_umem(&vnq->virtq);
out:
	return NULL;
}

/**
 * snap_virtio_net_destroy_queue() - Destroy Virtio net queue object
 * @vnq:       Virtio net queue
 *
 * Destroy and free a snap virtio net queue context.
 *
 * Return: Returns 0 on success.
 */
int snap_virtio_net_destroy_queue(struct snap_virtio_net_queue *vnq)
{
	int ret;

	vnq->virtq.virtq->consume_event = NULL;

	ret = snap_devx_obj_destroy(vnq->virtq.virtq);
	snap_virtio_teardown_virtq_umem(&vnq->virtq);

	return ret;
}

static int
snap_virtio_net_get_modifiable_virtq_fields(struct snap_virtio_net_queue *vnq)
{
	struct snap_virtio_net_queue_attr attr = {};
	int ret;

	ret = snap_virtio_net_query_queue(vnq, &attr);
	if (ret)
		return ret;

	vnq->virtq.mod_allowed_mask = attr.modifiable_fields;

	return 0;
}

/**
 * snap_virtio_net_modify_queue() - Modify a Virtio net queue object
 * @vnq:        snap Virtio net queue
 * @mask:       selected params to modify (mask of enum
 *              snap_virtio_net_queue_modify)
 * @attr:       attributes for the virtq modify
 *
 * Modify a Virtio net queue snap object according to a given mask.
 *
 * Return: 0 on success.
 */
int snap_virtio_net_modify_queue(struct snap_virtio_net_queue *vnq,
		uint64_t mask, struct snap_virtio_net_queue_attr *attr)
{
	int ret;

	if (!vnq->virtq.mod_allowed_mask) {
		ret = snap_virtio_net_get_modifiable_virtq_fields(vnq);
		if (ret)
			return ret;
	}

	return snap_virtio_modify_queue(&vnq->virtq, mask, &attr->vattr);
}
