#include <linux/init.h>
#include <linux/module.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <linux/videodev2.h>
#include <media/videobuf2-vmalloc.h>
#include <media/videobuf2-v4l2.h>
#include <linux/platform_device.h>
#include <linux/timer.h>


/* The minimum image width/height */
#define MIN_WIDTH  48
#define MIN_HEIGHT 32

#define MAX_WIDTH 1920
#define MAX_HEIGHT 1200

struct myvivi_fmt {
	u32	fourcc;          /* v4l2 format id */
	u8	buffers;
	u32	bit_depth;
	
	u32	colorspace;
};

struct myvivi_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head	list;
};

struct vivi {
	struct v4l2_device 		v4l2_dev;
	struct video_device 	vid_cap_dev;
	struct vb2_queue 		vb_vid_cap_q;
	const struct myvivi_fmt	*fmt_cap;
	
	u32 					vid_cap_caps;
	spinlock_t				slock;
	struct mutex 			mutex;
	
	struct v4l2_rect		fmt_cap_rect;
	enum v4l2_field			field_cap;
	unsigned				bytesperline;
	
	struct list_head		vid_cap_active;
	
	struct timer_list 		timer;
};

static struct vivi *myvivi;


struct myvivi_fmt myvivi_formats = {
	.fourcc   = V4L2_PIX_FMT_YUYV,
	.bit_depth = 16,
	.buffers = 1,
	.colorspace = V4L2_COLORSPACE_SRGB,
};

static void myvivi_timer_function(struct timer_list *t){
	struct vivi *vind = container_of(t, struct vivi, timer);
    struct myvivi_buffer *vid_cap_buf = NULL;
	void *vbuf;
	
	printk("------%s----\n",__func__);
	printk("width = %d,height = %d\n",vind->fmt_cap_rect.width,vind->fmt_cap_rect.height);
	
	if (!list_empty(&vind->vid_cap_active)) {
		vid_cap_buf = list_entry(vind->vid_cap_active.next, struct myvivi_buffer, list);
		if(vid_cap_buf->vb.vb2_buf.state != VB2_BUF_STATE_ACTIVE) {
			printk(KERN_ERR"buffer no active,error!!!\n");
			return;
		}
		list_del(&vid_cap_buf->list);
	}else {
		printk("No active queue to serve\n");
        goto out;
	}
    
	//取buf
	vbuf = vb2_plane_vaddr(&vid_cap_buf->vb.vb2_buf, 0);
	printk("bytesperline=%d\n",vind->bytesperline);
	
	//填充数据
	memset(vbuf,0xdd,vind->bytesperline * vind->fmt_cap_rect.height);
	
    // 它干两个工作，把buffer 挂入done_list 另一个唤醒应用层序，让它dqbuf
    vb2_buffer_done(&vid_cap_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
    
out:
    //修改timer的超时时间 : 30fps, 1秒里有30帧数据,每1/30 秒产生一帧数据
    mod_timer(&vind->timer, jiffies + HZ/30);
}



//表示它是一个摄像头设备
static int myvivi_vidoc_querycap(struct file *file,void *priv,
					struct v4l2_capability *cap) {
	struct vivi *vind = video_drvdata(file);

	strcpy(cap->driver, "myvivi");
	strcpy(cap->card, "myvivi");
	snprintf(cap->bus_info, sizeof(cap->bus_info),
			"platform:%s", vind->v4l2_dev.name);

	cap->capabilities = vind->vid_cap_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

//列举支持哪些格式
static int myvivi_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_fmtdesc *f){
	//struct vivi *vind = video_drvdata(file);
	const struct myvivi_fmt *fmt;

	printk("-----%s:line=%d, f->index=%d\n",__func__,__LINE__,f->index);
	
	if (f->index >= 1)
		return -EINVAL;

	fmt = &myvivi_formats;

	f->pixelformat = fmt->fourcc;
	return 0;
}

/* 返回当前所使用的格式 */
static int myvivi_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct vivi *vind = video_drvdata(file);

	printk("-----%s:line=%d\n",__func__,__LINE__);
	
	pix->width = vind->fmt_cap_rect.width;
	pix->height = vind->fmt_cap_rect.height;
	pix->pixelformat = vind->fmt_cap->fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = vind->fmt_cap->colorspace;
	pix->bytesperline = vind->bytesperline;
	pix->sizeimage = pix->bytesperline * pix->width;
	
	
	return 0;
}

//返回当前所使用的格式
static int myvivi_vidioc_try_fmt_vid_cap(struct file *file,void *priv, 
			struct v4l2_format *f) {
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct vivi *vind = video_drvdata(file);
	u32 w;
	u32 bit_depth;
	
	printk("-----%s:line=%d\n",__func__,__LINE__);

	v4l_bound_align_image(&pix->width, MIN_WIDTH, MAX_WIDTH, 2,
                  &pix->height, MIN_HEIGHT, MAX_HEIGHT, 0, 0);
	
	pix->pixelformat = vind->fmt_cap->fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = vind->fmt_cap->colorspace;
	w = pix->width;
	bit_depth = vind->fmt_cap->bit_depth;
	pix->bytesperline = (w * bit_depth) >> 3;   
	pix->sizeimage = pix->bytesperline * pix->width;
	
	
	
	return 0;
}

//设置数据格式
static int myvivi_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f) {
	struct v4l2_pix_format *pix = &f->fmt.pix;
	struct vivi *vind = video_drvdata(file);
	
	int ret = myvivi_vidioc_try_fmt_vid_cap(file, priv, f);
    if (ret < 0) {
		printk(KERN_ERR"try format error!!!\n");
        return ret;
	}
	
	vind->fmt_cap_rect.width = pix->width;
	vind->fmt_cap_rect.height = pix->height;
	vind->bytesperline = pix->bytesperline;
	
	return 0;
}

static int myvivi_vidioc_g_fbuf(struct file *file, void *fh, struct v4l2_framebuffer *a)
{
	return 0;
}

static int myvivi_vidioc_s_fbuf(struct file *file, void *fh, const struct v4l2_framebuffer *a)
{
	return 0;
}


static const struct v4l2_ioctl_ops myvivi_ioctl_ops = {
	//表示它是一个摄像头设备
	.vidioc_querycap = myvivi_vidoc_querycap,

	//用于列举、获取、测试、设置摄像头的数据格式
	.vidioc_enum_fmt_vid_cap 	= myvivi_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap 		= myvivi_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap 	= myvivi_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap 		= myvivi_vidioc_s_fmt_vid_cap,
	
	//.vidioc_enum_framesizes		= vidioc_enum_framesizes,
	.vidioc_g_fbuf			= myvivi_vidioc_g_fbuf,
	.vidioc_s_fbuf			= myvivi_vidioc_s_fbuf,

	//缓冲区操作: 申请/查询/放入/取出队列
	.vidioc_reqbufs 			= vb2_ioctl_reqbufs,
	.vidioc_querybuf 			= vb2_ioctl_querybuf,
	.vidioc_qbuf 				= vb2_ioctl_qbuf,
	.vidioc_dqbuf 				= vb2_ioctl_dqbuf,

	//启动/停止
	.vidioc_streamon 			= vb2_ioctl_streamon,
	.vidioc_streamoff 			= vb2_ioctl_streamoff,
};

static int myvivi_fop_release(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	
	if (vdev->queue)
		return vb2_fop_release(file);
	return v4l2_fh_release(file);
}

static const struct v4l2_file_operations myvivi_fops = {
	.owner			= THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = myvivi_fop_release,
	.poll			= vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
};

// vb2 核心层 vb2_reqbufs 中调用它，确定申请缓冲区的大小
static int vid_cap_queue_setup(struct vb2_queue *vq,
		       unsigned *nbuffers, unsigned *nplanes,
		       unsigned sizes[], struct device *alloc_devs[]){
	struct vivi *vind = vb2_get_drv_priv(vq);
	unsigned buffers = vind->fmt_cap->buffers;
	
	printk("width = %d \n",vind->fmt_cap_rect.width);
    printk("height = %d \n",vind->fmt_cap_rect.height);
    printk("pixelsize = %d \n",vind->bytesperline);
	printk("buffers = %d \n",buffers);
	
	sizes[0] = vind->bytesperline * vind->fmt_cap_rect.height;
	
	if (vq->num_buffers + *nbuffers < 2)
		*nbuffers = 2 - vq->num_buffers;
	
	*nplanes = buffers;
	
	printk("%s: count=%d\n", __func__, *nbuffers);
	
	return 0;
}

//APP调用ioctlVIDIOC_QBUF时导致此函数被调用
static int vid_cap_buf_prepare(struct vb2_buffer *vb){
	struct vivi *vind = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size;

	size = vind->bytesperline * vind->fmt_cap_rect.height;;

	if (vb2_plane_size(vb, 0) < size) {
		printk(KERN_ERR"%s data will not fit into plane (%lu < %lu)\n",
				__func__ ,vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);
	
	return 0;
}

static void vid_cap_buf_finish(struct vb2_buffer *vb) {
	
}

//APP调用ioctl VIDIOC_QBUF时
static void vid_cap_buf_queue(struct vb2_buffer *vb) {
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vivi *vind = vb2_get_drv_priv(vb->vb2_queue);
	struct myvivi_buffer *buf = container_of(vbuf, struct myvivi_buffer, vb);

	//printk("%s\n", __func__);

	spin_lock(&vind->slock);
	//把buf放入本地一个队列尾部,定时器处理函数就可以从本地队列取出videobuf
	list_add_tail(&buf->list, &vind->vid_cap_active);
	spin_unlock(&vind->slock);
}

static int vid_cap_start_streaming(struct vb2_queue *vq, unsigned count) {
	struct vivi *vind = vb2_get_drv_priv(vq);
	printk("------start timer-----\n");
	timer_setup(&vind->timer, myvivi_timer_function, 0);
	vind->timer.expires = jiffies + HZ/2;
	add_timer(&vind->timer);
	
	return 0;
}

static void vid_cap_stop_streaming(struct vb2_queue *vq) {
	struct vivi *vind = vb2_get_drv_priv(vq);

	printk("%s\n", __func__);
	del_timer(&vind->timer);
	
	/* Release all active buffers */
	while (!list_empty(&vind->vid_cap_active)) {
		struct myvivi_buffer *buf;

		buf = list_entry(vind->vid_cap_active.next,
				 struct myvivi_buffer, list);
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		printk("vid_cap buffer %d stop\n",buf->vb.vb2_buf.index);
	}
	
}

const struct vb2_ops myvivi_vid_cap_qops = {
	.queue_setup		= vid_cap_queue_setup,
	.buf_prepare		= vid_cap_buf_prepare,
	.buf_finish			= vid_cap_buf_finish,
	.buf_queue			= vid_cap_buf_queue,
	.start_streaming	= vid_cap_start_streaming,
	.stop_streaming		= vid_cap_stop_streaming,
};

static void myvivi_dev_release(struct v4l2_device *v4l2_dev)
{
	struct vivi *vind = container_of(v4l2_dev, struct vivi, v4l2_dev);

	v4l2_device_unregister(&vind->v4l2_dev);
	kfree(vind);
}

void video_device_release_empty(struct video_device *vdev) {

}

static int myvivi_probe(struct platform_device *pdev) {
	int ret = -1;
	struct vb2_queue *q;
	struct video_device *vfd;
	
	myvivi = kzalloc(sizeof(*myvivi), GFP_KERNEL);
	if (!myvivi)
		return -ENOMEM;
	
	/* 0.注册v4l2_dev */
	snprintf(myvivi->v4l2_dev.name, sizeof(myvivi->v4l2_dev.name),
			"%s-00", "myvivi");
	ret = v4l2_device_register(&pdev->dev,&myvivi->v4l2_dev);
	if (ret < 0) {
		printk(KERN_ERR"Failed to register v4l2_device: %d\n", ret);
		goto v4l2_dev_err;
	}
	myvivi->v4l2_dev.release = myvivi_dev_release;

	myvivi->vid_cap_caps = 	V4L2_CAP_VIDEO_CAPTURE | \
							V4L2_CAP_VIDEO_OVERLAY | \
							V4L2_CAP_STREAMING;
	
	myvivi->fmt_cap = &myvivi_formats;
	
	/* initialize locks */
	spin_lock_init(&myvivi->slock);
	mutex_init(&myvivi->mutex);

	/* init dma queues */
	INIT_LIST_HEAD(&myvivi->vid_cap_active);
	
	
	/* initialize vid_cap queue */
	q = &myvivi->vb_vid_cap_q;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->drv_priv = myvivi;
	q->buf_struct_size = sizeof(struct myvivi_buffer);
	q->ops = &myvivi_vid_cap_qops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->lock = &myvivi->mutex;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->dev = myvivi->v4l2_dev.dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto unreg_dev;
	
	vfd = &myvivi->vid_cap_dev;
	snprintf(vfd->name, sizeof(vfd->name),
		 "myvivi-00-vid-cap");
	vfd->fops = &myvivi_fops;
	vfd->ioctl_ops = &myvivi_ioctl_ops;
	vfd->device_caps = myvivi->vid_cap_caps;
	vfd->release = video_device_release_empty;
	vfd->v4l2_dev = &myvivi->v4l2_dev;
	vfd->queue = &myvivi->vb_vid_cap_q;
	vfd->lock = &myvivi->mutex;
	video_set_drvdata(vfd, myvivi);
	ret = video_register_device(vfd, VFL_TYPE_GRABBER, -1);
	if (ret < 0)
		goto unreg_dev;
	
	
	return ret;

unreg_dev:
	v4l2_device_put(&myvivi->v4l2_dev);
v4l2_dev_err:
	kfree(myvivi);

	return -1;
}

static int myvivi_remove(struct platform_device *pdev){
	printk("-----%s----\n",__func__);
	video_unregister_device(&myvivi->vid_cap_dev);
	v4l2_device_put(&myvivi->v4l2_dev);
	//kfree(myvivi);
	return 0;
}

static void myvivi_pdev_release(struct device *dev)
{
}

static struct platform_device myvivi_pdev = {
	.name			= "myvivi",
	.dev.release	= myvivi_pdev_release,
};

static struct platform_driver myvivi_pdrv = {
	.probe		= myvivi_probe,
	.remove		= myvivi_remove,
	.driver		= {
		.name	= "myvivi",
	},
};

static int __init myvivi_init(void)
{
	int ret;

	ret = platform_device_register(&myvivi_pdev);
	if (ret)
		return ret;

	ret = platform_driver_register(&myvivi_pdrv);
	if (ret)
		platform_device_unregister(&myvivi_pdev);

	return ret;
}

static void __exit myvivi_exit(void)
{
	platform_driver_unregister(&myvivi_pdrv);
	platform_device_unregister(&myvivi_pdev);
}

module_init(myvivi_init);
module_exit(myvivi_exit);
MODULE_LICENSE("GPL");


