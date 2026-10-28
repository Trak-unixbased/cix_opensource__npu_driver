// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2023-2025 Arm Technology (China) Co. Ltd. */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include "aipu_dma_buf.h"

static const struct dma_buf_ops aipu_dma_buf_ops;

/*
 * dma_buf_get() returns an ERR_PTR() on failure (never NULL), and the fd may
 * belong to any exporter: only dma-bufs exported by this driver carry a
 * struct aipu_dma_buf_priv in ->priv.
 */
static bool is_aipu_dma_buf(struct dma_buf *dmabuf)
{
	return !IS_ERR_OR_NULL(dmabuf) && dmabuf->ops == &aipu_dma_buf_ops && dmabuf->priv;
}

static u64 aipu_dma_buf_exec_id(struct aipu_memory_manager *mm)
{
	if (mm->has_iommu && mm->version >= AIPU_ISA_VERSION_ZHOUYI_V3_2) {
#if AIPU_USE_STANDARD_DMA_API_FOR_V3_2
		return 0;
#else
		return DMA_BUF_EXEC_ID;
#endif
	}
	return 0;
}

#if KERNEL_VERSION(4, 19, 0) > LINUX_VERSION_CODE
static int aipu_dma_buf_attach(struct dma_buf *dmabuf, struct device *dev,
			       struct dma_buf_attachment *attach)
#else
static int aipu_dma_buf_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
#endif
{
	return 0;
}

static void aipu_dma_buf_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attach)
{
}

/*
 * Build a scatterlist describing the CPU pages backing this buffer, then map
 * it for the importing device.
 *
 * The original code called dma_get_sgtable_attrs() with the *importer* device
 * and the NPU IOVA: dma-iommu then translated that IOVA in the importer's
 * IOMMU domain (garbage pages), and for a buffer carved out of a larger
 * region dma_common_find_pages() returned the pages of the region start.
 * It also cached one sg_table for every attachment and never unmapped it.
 */
static struct sg_table *aipu_map_dma_buf(struct dma_buf_attachment *attach,
					 enum dma_data_direction dir)
{
	struct aipu_dma_buf_priv *priv = attach->dmabuf->priv;
	struct device *npu = priv->mm->dev;
	unsigned int count = PAGE_ALIGN(priv->bytes) >> PAGE_SHIFT;
	struct sg_table *sgt = NULL;
	struct page **pages = NULL;
	unsigned int i = 0;
	int ret = 0;

	if (!count || !priv->va)
		return ERR_PTR(-EINVAL);

	pages = kvmalloc_array(count, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < count; i++) {
		void *va = (char *)priv->va + ((size_t)i << PAGE_SHIFT);

		pages[i] = is_vmalloc_addr(va) ? vmalloc_to_page(va) : virt_to_page(va);
		if (!pages[i]) {
			dev_err(npu, "dma-buf export: no page behind va %p\n", va);
			ret = -EFAULT;
			goto free_pages;
		}
	}

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto free_pages;
	}

	ret = sg_alloc_table_from_pages(sgt, pages, count, 0,
					(unsigned long)count << PAGE_SHIFT, GFP_KERNEL);
	if (ret)
		goto free_sgt;

	ret = dma_map_sgtable(attach->dev, sgt, dir, 0);
	if (ret) {
		dev_err(npu, "failed to map sgtable for the attached dev (%d)\n", ret);
		goto free_table;
	}

	kvfree(pages);
	return sgt;

free_table:
	sg_free_table(sgt);
free_sgt:
	kfree(sgt);
free_pages:
	kvfree(pages);
	return ERR_PTR(ret);
}

static void aipu_unmap_dma_buf(struct dma_buf_attachment *attach, struct sg_table *sgt,
			       enum dma_data_direction dir)
{
	if (IS_ERR_OR_NULL(sgt))
		return;

	dma_unmap_sgtable(attach->dev, sgt, dir, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static int aipu_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	int ret = 0;
	unsigned long vm_pgoff = 0;
	struct aipu_dma_buf_priv *priv = (struct aipu_dma_buf_priv *)dmabuf->priv;

	/*
	 * vm_pgoff is an offset inside the dma-buf (already range-checked by
	 * the dma-buf core): keep it instead of always mapping from offset 0.
	 */
	vm_pgoff = vma->vm_pgoff;
	vma->vm_pgoff = (priv->dev_pa >> PAGE_SHIFT) + vm_pgoff;
	ret = aipu_mm_mmap_buf(priv->mm, vma, NULL);
	vma->vm_pgoff = vm_pgoff;
	return ret;
}

#if KERNEL_VERSION(5, 11, 0) > LINUX_VERSION_CODE
static void *aipu_dma_vmap(struct dma_buf *dmabuf)
#elif KERNEL_VERSION(5, 18, 0) > LINUX_VERSION_CODE
static int aipu_dma_vmap(struct dma_buf *dmabuf, struct dma_buf_map *map)
#else
static int aipu_dma_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
#endif
{
	struct aipu_dma_buf_priv *priv = (struct aipu_dma_buf_priv *)dmabuf->priv;
#if KERNEL_VERSION(5, 11, 0) > LINUX_VERSION_CODE
	return priv->va;
#elif KERNEL_VERSION(5, 18, 0) > LINUX_VERSION_CODE
	dma_buf_map_set_vaddr(map, priv->va);
	return 0;
#else
	iosys_map_set_vaddr(map, priv->va);
	return 0;
#endif
}

#if KERNEL_VERSION(5, 11, 0) > LINUX_VERSION_CODE
static void aipu_dma_vunmap(struct dma_buf *dmabuf, void *vaddr)
#elif KERNEL_VERSION(5, 18, 0) > LINUX_VERSION_CODE
static void aipu_dma_vunmap(struct dma_buf *dmabuf, struct dma_buf_map *map)
#else
static void aipu_dma_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
#endif
{
}

/*
 * Called by the dma-buf core when the last reference (user fd, importer,
 * mmap) is dropped: this is the only safe place to give the NPU memory back.
 * The original code freed the memory and ->priv in AIPU_IOCTL_FREE_DMA_BUF
 * while the fd (and possibly importers) were still alive: use-after-free.
 */
static void aipu_dma_release(struct dma_buf *dmabuf)
{
	struct aipu_dma_buf_priv *priv = dmabuf->priv;
	struct aipu_buf_desc buf;

	if (!priv)
		return;

	memset(&buf, 0, sizeof(buf));
	buf.pa = priv->dev_pa;
	buf.dev_offset = priv->dev_pa;
	buf.bytes = priv->bytes;
	buf.region = AIPU_BUF_REGION_DEFAULT;
	buf.asid = AIPU_BUF_ASID_0;
	buf.exec_id = aipu_dma_buf_exec_id(priv->mm);

	if (aipu_free_dma_iova_phy(priv->mm, &buf, NULL))
		dev_warn(priv->mm->dev, "dma-buf release: free 0x%llx failed\n", priv->dev_pa);

	dmabuf->priv = NULL;
	kfree(priv);
}

#if ((KERNEL_VERSION(5, 6, 0) > LINUX_VERSION_CODE) && \
	(KERNEL_VERSION(4, 11, 12) < LINUX_VERSION_CODE))
static void *aipu_dma_map(struct dma_buf *dmabuf, unsigned long page_num)
{
	return NULL;
}
#endif

#if ((KERNEL_VERSION(4, 19, 0) > LINUX_VERSION_CODE) && \
	(KERNEL_VERSION(4, 11, 12) < LINUX_VERSION_CODE))
void *aipu_dma_map_atomic(struct dma_buf *dmabuf, unsigned long page_num)
{
	return NULL;
}
#endif

static const struct dma_buf_ops aipu_dma_buf_ops = {
	.attach = aipu_dma_buf_attach,
	.detach = aipu_dma_buf_detach,
	.map_dma_buf = aipu_map_dma_buf,
	.unmap_dma_buf = aipu_unmap_dma_buf,
	.mmap   = aipu_dma_buf_mmap,
	.vmap   = aipu_dma_vmap,
	.vunmap = aipu_dma_vunmap,
	.release = aipu_dma_release,
#if ((KERNEL_VERSION(5, 6, 0) > LINUX_VERSION_CODE) && \
	(KERNEL_VERSION(4, 11, 12) < LINUX_VERSION_CODE))
	.map = aipu_dma_map,
#endif
#if ((KERNEL_VERSION(4, 19, 0) > LINUX_VERSION_CODE) && \
	(KERNEL_VERSION(4, 11, 12) < LINUX_VERSION_CODE))
	.map_atomic = aipu_dma_map_atomic,
#endif
};

int aipu_alloc_dma_buf(struct aipu_memory_manager *mm, struct aipu_dma_buf_request *request)
{
	int ret = 0;
	int fd = 0;
	struct aipu_buf_request inter_req;
	struct aipu_dma_buf_priv *priv = NULL;
	char *va = NULL;
	struct dma_buf *dmabuf = NULL;
	struct aipu_phy_block *block = NULL;
	bool v3_2_iova = false;

	DEFINE_DMA_BUF_EXPORT_INFO(exp);

	if (!mm || !request || !request->bytes)
		return -EINVAL;

	memset(&inter_req, 0, sizeof(inter_req));
	inter_req.bytes = request->bytes;
	inter_req.align_in_page = 1;
	inter_req.region = AIPU_BUF_REGION_DEFAULT;
	inter_req.asid = AIPU_BUF_ASID_0;
	inter_req.reserve_iova_size = 0;
	inter_req.exec_id = 0;
	inter_req.alloc_mode = AIPU_DMA_BUF_MALLOC_DEFAULT;

	if (mm->version >= AIPU_ISA_VERSION_ZHOUYI_V3_2 && mm->has_iommu) {
#if !AIPU_USE_STANDARD_DMA_API_FOR_V3_2
		inter_req.exec_id = DMA_BUF_EXEC_ID;
		inter_req.alloc_mode = AIPU_DMA_BUF_MALLOC_IOVA_PHY;
		v3_2_iova = true;
#endif
	}

	ret = aipu_alloc_dma_iova_phy(mm, &inter_req, NULL);
	if (ret) {
		dev_err(mm->dev, "alloc phy memory fail.");
		return ret;
	}

	if (v3_2_iova) {
		block = aipu_get_block_buffer(mm, inter_req.desc.pa, "aipu_alloc_dma_buf");
		va = block ? block->va : NULL;
	} else {
		va = aipu_mm_get_va(mm, inter_req.desc.pa);
	}
	if (!va) {
		ret = -EFAULT;
		goto fail;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto fail;
	}

	priv->mm = mm;
	priv->dev_pa = inter_req.desc.pa;
	priv->dma_pa = inter_req.desc.pa;
	priv->bytes = inter_req.desc.bytes;
	priv->va = va;
	priv->sgt = NULL;

	exp.ops = &aipu_dma_buf_ops;
	exp.size = inter_req.desc.bytes;
	exp.flags = O_RDWR | O_CLOEXEC;
	exp.priv = priv;

	dmabuf = dma_buf_export(&exp);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto fail;
	}

	/* from here on, aipu_dma_release() owns priv and the NPU memory */
	if (block)
		block->dmabuf = dmabuf;

	fd = dma_buf_fd(dmabuf, exp.flags);
	if (fd < 0) {
		if (block)
			block->dmabuf = NULL;
		dma_buf_put(dmabuf);
		return fd;
	}

	request->fd = fd;
	return 0;

fail:
	kfree(priv);
	if (aipu_free_dma_iova_phy(mm, &inter_req.desc, NULL))
		dev_warn(mm->dev, "alloc dma-buf: rollback free failed\n");
	return ret;
}

/*
 * The buffer memory is released when the last reference to the dma-buf goes
 * away (see aipu_dma_release()), i.e. when user space closes the fd and every
 * importer has detached. This ioctl only validates the fd so that existing
 * user-mode drivers (FREE then close) keep working.
 */
int aipu_free_dma_buf(struct aipu_memory_manager *mm, int fd)
{
	struct dma_buf *dmabuf = NULL;
	int ret = 0;

	if (!mm || fd < 0)
		return -EINVAL;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		dev_err(mm->dev, "can't get dmabuf by fd %d.\n", fd);
		return -EINVAL;
	}

	if (!is_aipu_dma_buf(dmabuf))
		ret = -EINVAL;

	dma_buf_put(dmabuf);
	return ret;
}

int aipu_get_dma_buf_info(struct aipu_dma_buf *dmabuf_info)
{
	struct dma_buf *dmabuf = NULL;
	struct aipu_dma_buf_priv *priv = NULL;
	int ret = 0;

	if (!dmabuf_info || dmabuf_info->fd < 0)
		return -EINVAL;

	dmabuf = dma_buf_get(dmabuf_info->fd);
	if (IS_ERR(dmabuf))
		return -EINVAL;

	if (is_aipu_dma_buf(dmabuf)) {
		priv = (struct aipu_dma_buf_priv *)dmabuf->priv;
		dmabuf_info->pa = priv->dev_pa;
		dmabuf_info->bytes = priv->bytes;
	} else {
		ret = -EINVAL;
	}

	dma_buf_put(dmabuf);
	return ret;
}

int aipu_attach_dma_buf(struct aipu_memory_manager *mm, struct aipu_dma_buf *dmabuf_info)
{
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *table = NULL;
	struct aipu_dma_buf_importer *im_buf = NULL;
	struct scatterlist *sg = NULL;
	dma_addr_t next = 0;
	u64 bytes = 0;
	unsigned int i = 0;
	int ret = 0;

	if (!mm || !dmabuf_info || dmabuf_info->fd < 0)
		return -EINVAL;

	dmabuf = dma_buf_get(dmabuf_info->fd);
	if (IS_ERR(dmabuf))
		return -EINVAL;

	attach = dma_buf_attach(dmabuf, mm->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto put_buf;
	}

	/* the resv lock must be held around dma_buf_map_attachment() since 6.2 */
	table = dma_buf_map_attachment_unlocked(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(table)) {
		ret = table ? PTR_ERR(table) : -ENOMEM;
		goto detach;
	}

	/* the NPU needs one contiguous device address range */
	for_each_sgtable_dma_sg(table, sg, i) {
		if (i && sg_dma_address(sg) != next) {
			dev_err(mm->dev, "attach dma-buf: buffer is not contiguous for the NPU\n");
			ret = -EINVAL;
			goto unmap;
		}
		next = sg_dma_address(sg) + sg_dma_len(sg);
		bytes += sg_dma_len(sg);
	}
	if (!bytes) {
		ret = -EINVAL;
		goto unmap;
	}

	im_buf = kzalloc(sizeof(*im_buf), GFP_KERNEL);
	if (!im_buf) {
		ret = -ENOMEM;
		goto unmap;
	}

	dmabuf_info->pa = sg_dma_address(table->sgl);
	dmabuf_info->bytes = bytes;

	im_buf->fd = dmabuf_info->fd;
	im_buf->attach = attach;
	im_buf->table = table;
	mutex_lock(&mm->lock);
	list_add(&im_buf->node, &mm->importer_bufs->node);
	mutex_unlock(&mm->lock);

	/*
	 * Keep the reference taken by dma_buf_get(): an attachment does not pin
	 * the dma-buf, so closing the fd would otherwise free it under us.
	 * It is dropped in aipu_detach_dma_buf().
	 */
	return 0;

unmap:
	dma_buf_unmap_attachment_unlocked(attach, table, DMA_BIDIRECTIONAL);
detach:
	dma_buf_detach(dmabuf, attach);
put_buf:
	dma_buf_put(dmabuf);
	return ret;
}

static void aipu_dma_buf_importer_release(struct aipu_dma_buf_importer *im_buf)
{
	struct dma_buf *dmabuf = im_buf->attach->dmabuf;

	dma_buf_unmap_attachment_unlocked(im_buf->attach, im_buf->table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, im_buf->attach);
	dma_buf_put(dmabuf);
	kfree(im_buf);
}

int aipu_detach_dma_buf(struct aipu_memory_manager *mm, int fd)
{
	struct aipu_dma_buf_importer *im_buf = NULL;
	struct aipu_dma_buf_importer *next = NULL;
	struct aipu_dma_buf_importer *found = NULL;
	struct dma_buf *dmabuf = NULL;

	if (!mm || fd < 0)
		return -EINVAL;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return -EINVAL;

	mutex_lock(&mm->lock);
	list_for_each_entry_safe(im_buf, next, &mm->importer_bufs->node, node) {
		/* fd numbers can be reused: also match the dma-buf itself */
		if (fd == im_buf->fd && im_buf->attach->dmabuf == dmabuf) {
			list_del(&im_buf->node);
			found = im_buf;
			break;
		}
	}
	mutex_unlock(&mm->lock);

	if (found)
		aipu_dma_buf_importer_release(found);

	dma_buf_put(dmabuf);
	return found ? 0 : -EINVAL;
}

/**
 * @aipu_dma_buf_release_importers() - detach every imported dma-buf
 * @mm: pointer to memory manager struct initialized in aipu_init_mm()
 *
 * Called on driver teardown so that the references kept on foreign dma-bufs
 * are not leaked.
 */
void aipu_dma_buf_release_importers(struct aipu_memory_manager *mm)
{
	struct aipu_dma_buf_importer *im_buf = NULL;
	struct aipu_dma_buf_importer *next = NULL;
	LIST_HEAD(release);

	if (!mm || !mm->importer_bufs)
		return;

	mutex_lock(&mm->lock);
	list_splice_init(&mm->importer_bufs->node, &release);
	mutex_unlock(&mm->lock);

	list_for_each_entry_safe(im_buf, next, &release, node) {
		list_del(&im_buf->node);
		aipu_dma_buf_importer_release(im_buf);
	}
}

#if KERNEL_VERSION(6, 13, 0) <= LINUX_VERSION_CODE
MODULE_IMPORT_NS("DMA_BUF");
#elif KERNEL_VERSION(5, 4, 0) < LINUX_VERSION_CODE
MODULE_IMPORT_NS(DMA_BUF);
#endif
