/*
 * Copyright (C) 2006 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#include "config.h"

#include <sys/types.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/fcntl.h>
#include <sys/fnv_hash.h>

#include "fuse.h"
#include "fuse_node.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"

#include <sys/priv.h>
#include <security/mac/mac_framework.h>

static void
fuse_vnode_init(struct vnode *vp, struct fuse_vnode_data *fvdat,
    uint64_t nodeid, enum vtype vtyp)
{
    fvdat->nid = nodeid;
    if (nodeid == FUSE_ROOT_ID) {
        vp->v_vflag |= VV_ROOT;
    }
    vp->v_type = vtyp;
    vp->v_data = fvdat;
    mtx_init(&fvdat->createlock, "fuse node create mutex", NULL, MTX_DEF);
    sx_init(&fvdat->nodelock, "fuse node sx lock");
    sx_init(&fvdat->truncatelock, "fuse node truncate sx lock");
}

void
fuse_vnode_destroy(struct vnode *vp)
{
    struct fuse_vnode_data *fvdat = vp->v_data;

    vp->v_data = NULL;
    mtx_destroy(&fvdat->createlock);
    sx_destroy(&fvdat->nodelock);
    sx_destroy(&fvdat->truncatelock);
    free(fvdat, M_FUSEVN);
}

static int
fuse_vnode_cmp(struct vnode *vp, void *nidp)
{
    return (VTOI(vp) != *((uint64_t *)nidp));
}

static uint32_t __inline
fuse_vnode_hash(uint64_t id)
{
    return (fnv_32_buf(&id, sizeof(id), FNV1_32_INIT));
}

static int
fuse_vnode_alloc(struct mount *mp,
            struct thread *td,
            uint64_t nodeid,
            enum vtype vtyp,
	    int lkflags,
            struct vnode **vpp)
{
    struct fuse_vnode_data *fvdat;
    struct vnode *vp2;
    int err = 0;

    DEBUG("been asked for vno #%ju\n", (uintmax_t)nodeid);

    if (vtyp == VNON) {
        return EINVAL;
    }

    *vpp = NULL;
    err = vfs_hash_get(mp, fuse_vnode_hash(nodeid), lkflags, td, vpp,
        fuse_vnode_cmp, &nodeid);
    if (err)
        return (err);

    if (*vpp) {
        MPASS((*vpp)->v_type == vtyp && (*vpp)->v_data != NULL);
        DEBUG("vnode taken from hash\n");
        return (0);
    }

    lkflags = LK_EXCLUSIVE | LK_RETRY; /* XXXIP don't loose other flags */

    fvdat = malloc(sizeof(*fvdat), M_FUSEVN, M_WAITOK | M_ZERO);
    err = getnewvnode("fuse", mp, &fuse_vnops, vpp);
    if (err) {
        free(fvdat, M_FUSEVN);
        return (err);
    }

    vn_lock(*vpp, lkflags);
    err = insmntque(*vpp, mp);
    if (err) {
        free(fvdat, M_FUSEVN);
        return (err);
    }

    fuse_vnode_init(*vpp, fvdat, nodeid, vtyp);
    err = vfs_hash_insert(*vpp, fuse_vnode_hash(nodeid), lkflags,
        td, &vp2, fuse_vnode_cmp, &nodeid);

    if (err) {
        fuse_vnode_destroy(*vpp);
	*vpp = NULL;
        return (err);
    }

    /*
     * XXXIP: Prevent silent vnode reuse. It may happen because several fuse
     * filesystems ignore inode numbers
     */
    KASSERT(vp2 == NULL,
        ("vfs hash collision for node #%ju\n", (uintmax_t)nodeid));

    return (0);
}

int
fuse_vnode_get(struct mount         *mp,
               uint64_t              nodeid,
               struct vnode         *dvp,
               struct vnode         **vpp,
               struct componentname *cnp,
               enum vtype            vtyp,
               uint64_t              size)
{
    struct thread *td = (cnp != NULL ? cnp->cn_thread : curthread);
    int err = 0;

    debug_printf("dvp=%p\n", dvp);

    err = fuse_vnode_alloc(mp, td, nodeid, vtyp, LK_EXCLUSIVE | LK_RETRY, vpp);
    if (err) {
        return err;
    }

    if (cnp != NULL) {
        cache_enter(dvp, *vpp, cnp);
    }

    VTOFUD(*vpp)->nlookup++;

    return 0;
}