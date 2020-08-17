/*
 * modeset-atomic - DRM Atomic-API Modesetting Example
 * Written 2019 by Ezequiel Garcia <ezequiel@collabora.com>
 *
 * Dedicated to the Public Domain.
 */

/*
 * DRM Double-Buffered VSync'ed Atomic Modesetting Howto
 * This example extends modeset-vsync.c, introducing planes and the
 * atomic API.
 *
 * Planes can be used to blend or overlay images on top of a CRTC
 * framebuffer during the scanout process. Not all hardware provide
 * planes and the number of planes available is also limited. If there's
 * not enough planes available or the hardware does not provide them,
 * users should fallback to composition via GPU or CPU to blend or
 * overlay the planes. Notice that this render process will result
 * in delay, what justifies the usage of planes by modern hardware
 * that needs to be fast.
 *
 * There are three types of planes: primary, cursor and overlay. For
 * compatibility with legacy userspace, the default behavior is to expose
 * only overlay planes to userspace (we're going to see in the code that
 * we have to ask to receive all types of planes). A good example of plane
 * usage is this: imagine a static desktop screen and the user is moving
 * the cursor around. Only the cursor is moving. Instead of calculating the
 * complete scene for each time the user moves its cursor, we can update only
 * the cursor plane and it will be automatically overlayed by the hardware on
 * top of the primary plane. There's no need of software composition in this
 * case.
 *
 * But there was synchronisation problems related to multiple planes
 * usage. The KMS API was not atomic, so you'd have to update the primary
 * plane and then the overlay planes with distinct IOCTL's. This could lead
 * to tearing and also some trouble related to blocking, so the atomic
 * API was proposed to fix these problems.
 *
 * With the introduction of the KMS atomic API, all the planes could get
 * updated in a single IOCTL, using drmModeAtomicCommit(). This can be
 * either asynchronous or fully blocking.
 *
 * This example assumes that you are familiar with modeset-vsync. Only
 * the differences between both files are highlighted here.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/*
 * A new struct is introduced: drm_object. It stores properties of certain
 * objects (connectors, CRTC and planes) that are used in atomic modeset setup
 * and also in atomic page-flips (all planes updated in a single IOCTL).
 */

struct drm_object {
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	uint32_t id;
};

struct modeset_buf {
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;
	uint32_t fb;
};

struct modeset_output {
	struct modeset_output *next;

	unsigned int front_buf;
	struct modeset_buf bufs[2];

	struct drm_object connector;
	struct drm_object crtc;
	struct drm_object plane;

	drmModeModeInfo mode;
	uint32_t mode_blob_id;
	uint32_t crtc_index;

	bool pflip_pending;
	bool cleanup;

	uint8_t r, g, b;
	bool r_up, g_up, b_up;
};
static struct modeset_output *output_list = NULL;

/*
 * modeset_open() changes just a little bit. We now have to set that we're going
 * to use the KMS atomic API and check if the device is capable of handling it.
 */

static int modeset_open(int *out, const char *node)
{
	int fd, ret;
	uint64_t cap;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ret = -errno;
		fprintf(stderr, "cannot open '%s': %m\n", node);
		return ret;
	}

	/* Set that we want to receive all the types of planes in the list. This
	 * have to be done since, for legacy reasons, the default behavior is to
	 * expose only the overlay planes to the users. The atomic API only
	 * works if this is set.
	 */
	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) {
		fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
		return ret;
	}

	/* Here we set that we're going to use the KMS atomic API. It's supposed
	 * to set the DRM_CLIENT_CAP_UNIVERSAL_PLANES automatically, but it's a
	 * safe behavior to set it explicitly as we did in the previous
	 * commands. This is also good for learning purposes.
	 */
	ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "failed to set atomic cap, %d", ret);
		return ret;
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || !cap) {
		fprintf(stderr, "drm device '%s' does not support atomic KMS\n",
			node);
		close(fd);
		return -EOPNOTSUPP;
	}

	*out = fd;
	return 0;
}

/*
 * get_property_value() is a new function. Given a device, the properties of
 * an object and a name, search for the value of property 'name'. If we can't
 * find it, return -1.
 */

static int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props,
				  const char *name)
{
	drmModePropertyPtr prop;
	uint64_t value;
	bool found;
	int j;

	found = false;
	for (j = 0; j < props->count_props && !found; j++) {
		prop = drmModeGetProperty(fd, props->props[j]);
		if (!strcmp(prop->name, name)) {
			value = props->prop_values[j];
			found = true;
		}
		drmModeFreeProperty(prop);
	}

	if (!found)
		return -1;
	return value;
}

/*
 * get_drm_object_properties() is a new helpfer function that retrieves
 * the properties of a certain CRTC, plane or connector object.
 */

static void modeset_get_object_properties(int fd, struct drm_object *obj,
					  uint32_t type)
{
	const char *type_str;
	unsigned int i;

	obj->props = drmModeObjectGetProperties(fd, obj->id, type);
	if (!obj->props) {
		switch(type) {
			case DRM_MODE_OBJECT_CONNECTOR:
				type_str = "connector";
				break;
			case DRM_MODE_OBJECT_PLANE:
				type_str = "plane";
				break;
			case DRM_MODE_OBJECT_CRTC:
				type_str = "CRTC";
				break;
			default:
				type_str = "unknown type";
				break;
		}
		fprintf(stderr, "cannot get %s %d properties: %s\n",
			type_str, obj->id, strerror(errno));
		return;
	}

	obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
	for (i = 0; i < obj->props->count_props; i++)
		obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

/*
 * set_drm_object_property() is a new function. It sets a property value to a
 * CRTC, plane or connector object.
 */

static int set_drm_object_property(drmModeAtomicReq *req, struct drm_object *obj,
				   const char *name, uint64_t value)
{
	int i;
	uint32_t prop_id = 0;

	for (i = 0; i < obj->props->count_props; i++) {
		if (!strcmp(obj->props_info[i]->name, name)) {
			prop_id = obj->props_info[i]->prop_id;
			break;
		}
	}

	if (prop_id == 0) {
		fprintf(stderr, "no object property: %s\n", name);
		return -EINVAL;
	}

	return drmModeAtomicAddProperty(req, obj->id, prop_id, value);
}

/*
 * modeset_find_crtc() changes a little bit. Now we also have to save the CRTC
 * index, and not only its id.
 */

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_output *out)
{
	drmModeEncoder *enc;
	unsigned int i, j;
	uint32_t crtc;
	struct modeset_output *iter;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (iter = output_list; iter; iter = iter->next) {
				if (iter->crtc.id == crtc) {
					crtc = 0;
					break;
				}
			}

			if (crtc > 0) {
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC.
	 */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
				i, conn->encoders[i], errno);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other output already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = output_list; iter; iter = iter->next) {
				if (iter->crtc.id == crtc) {
					crtc = 0;
					break;
				}
			}

			/* We have found a CRTC, so save it and return. Note
			 * that we have to save its index as well. The CRTC
			 * index (not its ID) will be used when searching for a
			 * suitable plane.
			 */
			if (crtc > 0) {
				fprintf(stdout, "crtc %u found for encoder %u, will need full modeset\n",
					crtc, conn->encoders[i]);;
				drmModeFreeEncoder(enc);
				out->crtc.id = crtc;
				out->crtc_index = j;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable crtc for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

/*
 * modeset_find_plane() is a new function. Given a certain combination
 * of connector+CRTC, it looks for a primary plane for it.
 */

static int modeset_find_plane(int fd, struct modeset_output *out)
{
	drmModePlaneResPtr plane_res;
	bool found_primary = false;
	int i, ret = -EINVAL;

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
				strerror(errno));
		return -ENOENT;
	}

	/* iterates through all planes of a certain device */
	for (i = 0; (i < plane_res->count_planes) && !found_primary; i++) {
		int plane_id = plane_res->planes[i];

		drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
		if (!plane) {
			fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id,
					strerror(errno));
			continue;
		}

		/* check if the plane can be used by our CRTC */
		if (plane->possible_crtcs & (1 << out->crtc_index)) {
			drmModeObjectPropertiesPtr props =
				drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

			/* Get the "type" property to check if this is a primary
			 * plane. Type property is special, as its enum value is
			 * defined in UAPI headers. For the properties that are
			 * not defined in the UAPI headers, we would have to
			 * give kernel the property name and it would return the
			 * corresponding enum value. We could also do this for
			 * the "type" property, but it would make this simple
			 * example more complex. The reason why defining enum
			 * values for kernel properties in UAPI headers is
			 * deprecated is that string names are easier to both
			 * (userspace and kernel) make unique and keep
			 * consistent between drivers and kernel versions. But
			 * in order to not break userspace, some properties were
			 * left in the UAPI headers as well.
			 */
			if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY) {
				found_primary = true;
				out->plane.id = plane_id;
				ret = 0;
			}

			drmModeFreeObjectProperties(props);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);

	if (found_primary)
		fprintf(stdout, "found primary plane, id: %d\n", out->plane.id);
	else
		fprintf(stdout, "couldn't find a primary plane\n");
	return ret;
}

/*
 * modeset_drm_object_fini() is a new helper function that destroys CRTCs,
 * connectors and planes
 */

static void modeset_drm_object_fini(struct drm_object *obj)
{
	for (int i = 0; i < obj->props->count_props; i++)
		drmModeFreeProperty(obj->props_info[i]);
	free(obj->props_info);
	drmModeFreeObjectProperties(obj->props);
}

/*
 * modeset_setup_objects() is a new function. It helps us to retrieve
 * connector, CRTC and plane objects properties from the device. These
 * properties will help us during the atomic modesetting commit, so we save
 * them in our struct modeset_output object.
 */

static int modeset_setup_objects(int fd, struct modeset_output *out)
{
	struct drm_object *connector = &out->connector;
	struct drm_object *crtc = &out->crtc;
	struct drm_object *plane = &out->plane;

	/* retrieve connector properties from the device */
	modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
	if (!connector->props)
		goto out_conn;

	/* retrieve CRTC properties from the device */
	modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
	if (!crtc->props)
		goto out_crtc;

	/* retrieve plane properties from the device */
	modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
	if (!plane->props)
		goto out_plane;

	return 0;

out_plane:
	modeset_drm_object_fini(crtc);
out_crtc:
	modeset_drm_object_fini(connector);
out_conn:
	return -ENOMEM;
}

/*
 * modeset_destroy_objects() is a new function. It destroys what we allocate
 * in modeset_setup_objects().
 */

static void modeset_destroy_objects(int fd, struct modeset_output *out)
{
	modeset_drm_object_fini(&out->connector);
	modeset_drm_object_fini(&out->crtc);
	modeset_drm_object_fini(&out->plane);
}

/*
 * modeset_create_fb() stays the same.
 */

static int modeset_create_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = buf->width;
	creq.height = buf->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer (%d): %m\n",
			errno);
		return -errno;
	}
	buf->stride = creq.pitch;
	buf->size = creq.size;
	buf->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	handles[0] = buf->handle;
	pitches[0] = buf->stride;
	ret = drmModeAddFB2(fd, buf->width, buf->height, DRM_FORMAT_XRGB8888,
			    handles, pitches, offsets, &buf->fb, 0);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = buf->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* perform actual memory mapping */
	buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		        fd, mreq.offset);
	if (buf->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer (%d): %m\n",
			errno);
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(buf->map, 0, buf->size);

	return 0;

err_fb:
	drmModeRmFB(fd, buf->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

/*
 * modeset_destroy_fb() stays the same.
 */

static void modeset_destroy_fb(int fd, struct modeset_buf *buf)
{
	struct drm_mode_destroy_dumb dreq;

	/* unmap buffer */
	munmap(buf->map, buf->size);

	/* delete framebuffer */
	drmModeRmFB(fd, buf->fb);

	/* delete dumb buffer */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = buf->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

/*
 * modeset_setup_framebuffers() creates framebuffers for the back and front
 * buffers of a certain output. Also, it copies the connector mode to these
 * buffers.
 */

static int modeset_setup_framebuffers(int fd, drmModeConnector *conn,
				      struct modeset_output *out)
{
	int i, ret;

	/* setup the front and back framebuffers */
	for (i = 0; i < 2; i++) {

		/* copy mode info to buffer */
		out->bufs[i].width = conn->modes[0].hdisplay;
		out->bufs[i].height = conn->modes[0].vdisplay;

		/* create a framebuffer for the buffer */
		ret = modeset_create_fb(fd, &out->bufs[i]);
		if (ret) {
			/* the second framebuffer creation failed, so
			 * we have to destroy the first before returning */
			if (i == 1)
				modeset_destroy_fb(fd, &out->bufs[0]);
			return ret;
		}
	}

	return 0;
}

/*
 * modeset_output_destroy() is new. It destroys the objects (connector, crtc and
 * plane), front and back buffers, the mode blob property and then destroys the
 * output itself.
 */

static void modeset_output_destroy(int fd, struct modeset_output *out)
{
	/* destroy connector, crtc and plane objects */
	modeset_destroy_objects(fd, out);

	/* destroy front/back framebuffers */
	modeset_destroy_fb(fd, &out->bufs[0]);
	modeset_destroy_fb(fd, &out->bufs[1]);

	/* destroy mode blob property */
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);

	free(out);
}

/*
 * With a certain combination of connector+CRTC, we look for a suitable primary
 * plane for it. After that, we retrieve connector, CRTC and plane objects
 * properties from the device. These objects are used during the atomic modeset
 * setup (see modeset_atomic_prepare_commit()) and also during the page-flips
 * (see modeset_draw_out() and modeset_atomic_commit()).
 *
 * Besides that, we have to create a blob property that receives the output
 * mode. When we perform an atomic commit, the driver expects a CRTC property
 * named "MODE_ID", which points to the id of a blob. This usually happens for
 * properties that are not simple types. In this particular case, out->mode is a
 * struct. But we could have another property that expects the id of a blob that
 * holds an array, for instance.
 */

static struct modeset_output *modeset_output_create(int fd, drmModeRes *res,
						    drmModeConnector *conn)
{
	int ret;
	struct modeset_output *out;

	/* creates an output structure */
	out = malloc(sizeof(*out));
	memset(out, 0, sizeof(*out));
	out->connector.id = conn->connector_id;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		goto out_error;
	}

	/* copy the mode information into our output structure */
	memcpy(&out->mode, &conn->modes[0], sizeof(out->mode));
	/* create the blob property using out->mode and save its id in the output*/
	if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode),
	                              &out->mode_blob_id) != 0) {
		fprintf(stderr, "couldn't create a blob property\n");
		goto out_error;
	}
	fprintf(stderr, "mode for connector %u is %ux%u\n",
	        conn->connector_id, out->bufs[0].width, out->bufs[0].height);

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, out);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		goto out_blob;
	}

	/* with a connector and crtc, find a primary plane */
	ret = modeset_find_plane(fd, out);
	if (ret) {
		fprintf(stderr, "no valid plane for crtc %u\n", out->crtc.id);
		goto out_blob;
	}

	/* gather properties of our connector, CRTC and planes */
	ret = modeset_setup_objects(fd, out);
	if (ret) {
		fprintf(stderr, "cannot get plane properties\n");
		goto out_blob;
	}

	/* setup front/back framebuffers for this CRTC */
	ret = modeset_setup_framebuffers(fd, conn, out);
	if (ret) {
		fprintf(stderr, "cannot create framebuffers for connector %u\n",
			conn->connector_id);
		goto out_obj;
	}

	return out;

out_obj:
	modeset_destroy_objects(fd, out);
out_blob:
	drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
out_error:
	free(out);
	return NULL;
}

/*
 * modeset_prepare() changes a little bit. Now we use the new function
 * modeset_output_create() to allocate memory and setup the output.
 */

static int modeset_prepare(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	unsigned int i;
	struct modeset_output *out;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "cannot retrieve DRM resources (%d): %m\n",
			errno);
		return -errno;
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
				i, res->connectors[i], errno);
			continue;
		}

		/* create an output structure and free connector data */
		out = modeset_output_create(fd, res, conn);
		drmModeFreeConnector(conn);
		if (!out)
			continue;

		/* link output into global list */
		out->next = output_list;
		output_list = out;
	}
	if (!output_list) {
		fprintf(stderr, "couldn't create any outputs\n");
		return -1;
	}

	/* free resources again */
	drmModeFreeResources(res);
	return 0;
}

/*
 * modeset_atomic_prepare_commit() is new. Here we set the values of properties
 * (of our connector, CRTC and plane objects) that we want to change in the
 * atomic commit. These changes are temporarily stored in drmModeAtomicReq *req
 * until the commit actually happens.
 */

static int modeset_atomic_prepare_commit(int fd, struct modeset_output *out,
					 drmModeAtomicReq *req)
{
	struct drm_object *plane = &out->plane;
	struct modeset_buf *buf = &out->bufs[out->front_buf ^ 1];

	/* set id of the CRTC id that the connector is using */
	if (set_drm_object_property(req, &out->connector, "CRTC_ID", out->crtc.id) < 0)
		return -1;

	/* set the mode id of the CRTC; this property receives the id of a blob
	 * property that holds the struct that actually contains the mode info */
	if (set_drm_object_property(req, &out->crtc, "MODE_ID", out->mode_blob_id) < 0)
		return -1;

	/* set the CRTC object as active */
	if (set_drm_object_property(req, &out->crtc, "ACTIVE", 1) < 0)
		return -1;

	/* set properties of the plane related to the CRTC and the framebuffer */
	if (set_drm_object_property(req, plane, "FB_ID", buf->fb) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_ID", out->crtc.id) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_W", buf->width << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "SRC_H", buf->height << 16) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_X", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_Y", 0) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_W", buf->width) < 0)
		return -1;
	if (set_drm_object_property(req, plane, "CRTC_H", buf->height) < 0)
		return -1;

	return 0;
}

/*
 * A short helper function to compute a changing color value. No need to
 * understand it.
 */

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod)
{
	uint8_t next;

	next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}

/*
 * Draw on back framebuffer before the page-flip is requested.
 */

static void modeset_paint_framebuffer(struct modeset_output *out)
{
	struct modeset_buf *buf;
	unsigned int j, k, off;

	/* draw on back framebuffer */
	out->r = next_color(&out->r_up, out->r, 5);
	out->g = next_color(&out->g_up, out->g, 5);
	out->b = next_color(&out->b_up, out->b, 5);
	buf = &out->bufs[out->front_buf ^ 1];
	for (j = 0; j < buf->height; ++j) {
		for (k = 0; k < buf->width; ++k) {
			off = buf->stride * j + k * 4;
			*(uint32_t*)&buf->map[off] =
				     (out->r << 16) | (out->g << 8) | out->b;
		}
	}

}

/*
 * modeset_draw_out() prepares the framebuffer with the drawing and then it asks
 * for the driver to perform an atomic commit. This will lead to a page-flip and
 * the content of the framebuffer will be displayed. In this simple example
 * we're only using the primary plane, but we could also be updating other
 * planes in the same atomic commit.
 *
 * Just like in modeset_perform_modeset(), we first setup everything with
 * modeset_atomic_prepare_commit() and then actually perform the atomic commit.
 * But there are some important differences:
 *
 * 1. Here we just want to perform a commit that changes the state of a specific
 *    output, and in modeset_perform_modeset() we did an atomic commit that was
 *    supposed to setup all the outputs at once. So there's no need to prepare
 *    every output before performing the atomic commit. But let's suppose you
 *    prepare every output and then perform the commit. It should schedule a
 *    page-flip for all of them, but modeset_draw_out() was called because the
 *    page-flip for a specific output has finished. The others may not be
 *    prepared for a page-flip yet (e.g. in the middle of a scanout), so these
 *    page-flips will fail.
 *
 * 2. Here we have already painted the framebuffer and also we don't use the
 *    flag DRM_MODE_ALLOW_MODESET anymore, since the modeset already happened.
 *    We could continue to use this flag, as it makes no difference if
 *    modeset_perform_modeset() is correct and there's no bug in the kernel.
 *    The flag only allows (it doesn't force) the driver to perform a modeset,
 *    but we have already performed it in modeset_perform_modeset() and now we
 *    just want page-flips to occur. If we still need to perform modesets it
 *    means that we have a bug somewhere, and it may be better to fail than to
 *    glitch (a modeset can cause unecessary latency and also blank the screen).
 */

static void modeset_draw_out(int fd, struct modeset_output *out)
{
	drmModeAtomicReq *req;
	int ret, flags;

	/* draw on framebuffer of the output */
	modeset_paint_framebuffer(out);

	/* prepare output for atomic commit */
	req = drmModeAtomicAlloc();
	ret = modeset_atomic_prepare_commit(fd, out, req);
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed, %d\n", errno);
		return;
	}

	/* We've just draw on the framebuffer, prepared the commit and now it's
	 * time to perform a page-flip to display its content.
	 *
	 * DRM_MODE_PAGE_FLIP_EVENT signalizes that we want to receive a
	 * page-flip event in the DRM-fd when the page-flip happens. This flag
	 * is also used in the non-atomic examples, so you're probably familiar
	 * with it.
	 *
	 * DRM_MODE_ATOMIC_NONBLOCK makes the page-flip non-blocking. We don't
	 * want to be blocked waiting for the commit to happen, since we can use
	 * this time to prepare a new framebuffer, for instance. We can only do
	 * this because there are mechanisms to know when the commit is complete
	 * (like page flip event, explained above).
	 */
	flags = DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	drmModeAtomicFree(req);

	if (ret < 0) {
		fprintf(stderr, "atomic commit failed, %d\n", errno);
		return;
	}
	out->front_buf ^= 1;
	out->pflip_pending = true;
}

/*
 * modeset_page_flip_event() changes. Now that we are using page_flip_handler2,
 * we also receive the CRTC that is responsible for this event. When using the
 * atomic API we commit multiple CRTC's at once, so we need the information of
 * what output caused the event in order to schedule a new page-flip for it.
 */

static void modeset_page_flip_event(int fd, unsigned int frame,
				    unsigned int sec, unsigned int usec,
				    unsigned int crtc_id, void *data)
{
	struct modeset_output *out, *iter;

	/* find the output responsible for this event */
	out = NULL;
	for (iter = output_list; iter; iter = iter->next) {
		if (iter->crtc.id == crtc_id) {
			out = iter;
			break;
		}
	}
	if (out == NULL)
		return;

	out->pflip_pending = false;
	if (!out->cleanup)
		modeset_draw_out(fd, out);
}

/*
 * modeset_perform_modeset() is new. First we define what properties have to be
 * changed and the values that they will receive. To check if the modeset will
 * work as expected, we perform an atomic commit with the flag
 * DRM_MODE_ATOMIC_TEST_ONLY. With this flag the DRM driver tests if the atomic
 * commit would work, but it doesn't commit it to the hardware. After, the same
 * atomic commit is performed without the TEST_ONLY flag, but not only before we
 * draw on the framebuffers of the outputs. This is necessary to avoid
 * displaying unwanted content.
 *
 * NOTE: we can't perform an atomic commit without an attached frambeuffer
 * (even when we have DRM_MODE_ATOMIC_TEST_ONLY). It will simply fail.
 */

static int modeset_perform_modeset(int fd)
{
	int ret, flags;
	struct modeset_output *iter;
	drmModeAtomicReq *req;

	/* prepare modeset on all outputs */
	req = drmModeAtomicAlloc();
	for (iter = output_list; iter; iter = iter->next) {
		ret = modeset_atomic_prepare_commit(fd, iter, req);
		if (ret < 0)
			break;
	}
	if (ret < 0) {
		fprintf(stderr, "prepare atomic commit failed, %d\n", errno);
		return ret;
	}

	/* perform test-only atomic commit */
	flags = DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0) {
		fprintf(stderr, "test-only atomic commit failed, %d\n", errno);
		drmModeAtomicFree(req);
		return ret;
	}

	/* draw on back framebuffer of all outputs */
	for (iter = output_list; iter; iter = iter->next) {

		/* colors initialization, this is the first time we're drawing */
		iter->r = rand() % 0xff;
		iter->g = rand() % 0xff;
		iter->b = rand() % 0xff;
		iter->r_up = iter->g_up = iter->b_up = true;

		modeset_paint_framebuffer(iter);
	}

	/* initial modeset on all outputs */
	flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	if (ret < 0)
		fprintf(stderr, "modeset atomic commit failed, %d\n", errno);

	drmModeAtomicFree(req);

	return ret;
}

/*
 * modeset_draw() changes. If we got here, the modeset already occurred. When
 * the page-flip for a certain output is done, an event will be fired and we'll
 * be able to handle it.
 *
 * Here we define the function that should handle these events, which is
 * modeset_page_flip_event(). This function calls modeset_draw_out(), which is
 * responsible for preparing a new framebuffer and performing another atomic
 * commit for us.
 *
 * Then we have a 5 seconds loop that keeps waiting for the events that are
 * fired when the page-flip is complete. drmHandleEvent() is reponsible for
 * reading the events from the fd and to call modeset_page_flip_event() for
 * each one of them.
 */

static void modeset_draw(int fd)
{
	int ret;
	fd_set fds;
	time_t start, cur;
	struct timeval v;
	drmEventContext ev;

	/* init variables */
	srand(time(&start));
	FD_ZERO(&fds);
	memset(&v, 0, sizeof(v));
	memset(&ev, 0, sizeof(ev));

	/* 3 is the first version that allow us to use page_flip_handler2, which
	 * is just like page_flip_handler but with the addition of passing the
	 * crtc_id as argument to the function that will handle page-flip events
	 * (in our case, modeset_page_flip_event()). This is good because we can
	 * find out for what output the page-flip happened.
	 *
	 * The usage of page_flip_handler2 is the reason why we needed to verify
	 * the support for DRM_CAP_CRTC_IN_VBLANK_EVENT.
	 */
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	/* perform modeset using atomic commit */
	modeset_perform_modeset(fd);

	/* wait 5s for VBLANK or input events */
	while (time(&cur) < start + 5) {
		FD_SET(0, &fds);
		FD_SET(fd, &fds);
		v.tv_sec = start + 5 - cur;

		ret = select(fd + 1, &fds, NULL, NULL, &v);
		if (ret < 0) {
			fprintf(stderr, "select() failed with %d: %m\n", errno);
			break;
		} else if (FD_ISSET(0, &fds)) {
			fprintf(stderr, "exit due to user-input\n");
			break;
		} else if (FD_ISSET(fd, &fds)) {
			/* read the fd looking for events and handle each event
			 * by calling modeset_page_flip_event() */
			drmHandleEvent(fd, &ev);
		}
	}
}

/*
 * modeset_cleanup() stays the same.
 */

static void modeset_cleanup(int fd)
{
	struct modeset_output *iter;
	drmEventContext ev;
	int ret;

	/* init variables */
	memset(&ev, 0, sizeof(ev));
	ev.version = 3;
	ev.page_flip_handler2 = modeset_page_flip_event;

	while (output_list) {
		/* get first output from list */
		iter = output_list;

		/* if a page-flip is pending, wait for it to complete */
		iter->cleanup = true;
		fprintf(stderr, "wait for pending page-flip to complete...\n");
		while (iter->pflip_pending) {
			ret = drmHandleEvent(fd, &ev);
			if (ret)
				break;
		}

		/* move head of the list to the next output */
		output_list = iter->next;

		/* destroy current output */
		modeset_output_destroy(fd, iter);
	}
}

/*
 * main() also changes. Instead of performing the KMS setup calling
 * drmModeSetCrtc(), we instead setup it using the atomic API with the
 * function modeset_perform_modeset(), which is called by modeset_draw().
 */

int main(int argc, char **argv)
{
	int ret, fd;
	const char *card;

	/* check which DRM device to open */
	if (argc > 1)
		card = argv[1];
	else
		card = "/dev/dri/card0";

	fprintf(stderr, "using card '%s'\n", card);

	/* open the DRM device */
	ret = modeset_open(&fd, card);
	if (ret)
		goto out_return;

	/* prepare all connectors and CRTCs */
	ret = modeset_prepare(fd);
	if (ret)
		goto out_close;

	/* draw some colors for 5seconds */
	modeset_draw(fd);

	/* cleanup everything */
	modeset_cleanup(fd);

	ret = 0;

out_close:
	close(fd);
out_return:
	if (ret) {
		errno = -ret;
		fprintf(stderr, "modeset failed with error %d: %m\n", errno);
	} else {
		fprintf(stderr, "exiting\n");
	}
	return ret;
}

/*
 * This is a very simple example to show how to use the KMS atomic API and how
 * different it is from legacy KMS. Most modern drivers are using the atomic
 * API, so it is important to have this example.
 *
 * Just like vsync'ed double-buffering, the atomic API does not not solve all
 * the problems that can happen  and you have to figure out the best
 * implementation for your use case.
 *
 * If you want to take a look at more complex examples that makes use of KMS
 * atomic API, I can recommend you:
 *
 * - kms-quads: https://gitlab.freedesktop.org/daniels/kms-quads/
 *   A more complex (but also well-explained) example that can be used to
 *   learn how to build a compositor using the atomic API. Supports both
 *   GL and software rendering.
 *
 * - Weston: https://gitlab.freedesktop.org/wayland/weston
 *   Reference implementation of a Wayland compositor. It's a very
 *   sophisticated DRM renderer, hard to understand fully as it uses more
 *   complicated techniques like DRM planes.
 *
 * Any feedback is welcome. Feel free to use this code freely for your own
 * documentation or projects.
 *
 *  - Hosted on http://github.com/dvdhrm/docs
 */
