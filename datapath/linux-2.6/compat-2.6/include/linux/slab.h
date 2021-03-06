#ifndef __LINUX_SLAB_WRAPPER_H
#define __LINUX_SLAB_WRAPPER_H 1

#include_next <linux/slab.h>

#ifndef HAVE_KMEMDUP
extern void *kmemdup(const void *src, size_t len, gfp_t gfp);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
#define kmem_cache_create(n, s, a, f, c) kmem_cache_create(n, s, a, f, c, NULL)
#endif

#endif
