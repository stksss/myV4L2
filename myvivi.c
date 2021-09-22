#include <linux/init.h>
#include <linux/module.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <linux/videodev2.h>
#include <media/videobuf-vmalloc.h>
#include <linux/platform_device.h>


struct vivi {
	struct v4l2_device v4l2_dev;
	struct video_device *vdev;
	struct device dev;
	struct v4l2_format format;
	
	struct videobuf_queue vb_vidqueue;
	spinlock_t queue_slock;
};


static struct vivi *myvivi;

//表示它是一个摄像头设备
static int myvivi_vidoc_querycap(struct file *file,void *priv,
					struct v4l2_capability *cap) {
	strcpy(cap->driver, "myvivi");
	strcpy(cap->card,"myvivi");
	cap->version = 0x0001;
	cap->capabilities = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

//列举支持哪些格式
static int myvivi_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_fmtdesc *f){
	if(f->index > 1) return -ENOMEM;
	
	strcpy(f->description, "4:2:2, packed, YUYV");
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	
	return 0;
}

/* 返回当前所使用的格式 */
static int myvivi_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
    memcpy(f, &myvivi->format, sizeof(myvivi->format));
	return 0;
}

//返回当前所使用的格式
static int myvivi_vidioc_try_fmt_vid_cap(struct file *file,void *priv, 
			struct v4l2_format *f) {
	unsigned int maxw, maxh;
	enum v4l2_field field;
	
	if(f->fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) 
		return -EINVAL;
	
	field = f->fmt.pix.field;
	
	if(field == V4L2_FIELD_ANY) {
		field = V4L2_FIELD_INTERLACED;
	} else if(V4L2_FIELD_INTERLACED != field) {
		return -EINVAL;
	}
	
	maxw = 1024;
	maxh = 788;
	
	//调整format的width, height
	v4l_bound_align_image(&f->fmt.pix.width,48,maxw,2,
					&f->fmt.pix.height,32,maxh,0,0);
	//计算一行大小, 单位字节
	f->fmt.pix.bytesperline = (f->fmt.pix.width * 16) >> 3;
	//计算一帧大小
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	
	return 0;
}

//设置数据格式
static int myvivi_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f) {
	int ret = myvivi_vidioc_try_fmt_vid_cap(file,NULL,f);
	if(ret < 0) {
		printk(KERN_ERR"try format video capture error!!!\n");
		return ret;
	}
	
	//直接拷贝
	memcpy(&myvivi->format,f,sizeof(myvivi->format));
	
	return ret;
}

static int myvivi_vidioc_reqbufs(struct file *file, void *priv, 
			struct v4l2_requestbuffers *p) {
	return videobuf_reqbufs(&myvivi->vb_vidqueue,p);
}

static int myvivi_vidioc_querybuf(struct file *file, void *priv,
			struct v4l2_buffer *p) {
	return videobuf_querybuf(&myvivi->vb_vidqueue,p);
}

static int myvivi_vidioc_qbuf(struct file *file, void *priv,
			struct v4l2_buffer *p){
	return videobuf_qbuf(&myvivi->vb_vidqueue,p);
}

static int myvivi_vidioc_dqbuf(struct file *file, void *priv, 
			struct v4l2_buffer *p) {
	return videobuf_dqbuf(&myvivi->vb_vidqueue, p, 
				file->f_flags & O_NONBLOCK);
}

static const struct v4l2_ioctl_ops myvivi_ioctl_ops = {
	//表示它是一个摄像头设备
	.vidioc_querycap = myvivi_vidoc_querycap,

	//用于列举、获取、测试、设置摄像头的数据格式
	.vidioc_enum_fmt_vid_cap 	= myvivi_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap 		= myvivi_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap 	= myvivi_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap 		= myvivi_vidioc_s_fmt_vid_cap,

	//缓冲区操作: 申请/查询/放入/取出队列
	.vidioc_reqbufs 			= myvivi_vidioc_reqbufs,
	.vidioc_querybuf 			= myvivi_vidioc_querybuf,
	.vidioc_qbuf 				= myvivi_vidioc_qbuf,
	.vidioc_dqbuf 				= myvivi_vidioc_dqbuf,

//	//启动/停止
//	.vidioc_streamon 			= myvivi_vidioc_streamon,
//	.vidioc_streamoff 			= myvivi_vidioc_streamoff,
};

/*
 * APP调用ioctl VIDIOC_REQUFS时会调用此函数
 * 重新调整count和size
*/
static int myvivi_buffer_setup(struct videobuf_queue *vq, 
			unsigned int *count, unsigned int *size) {
	*size = myvivi->format.fmt.pix.sizeimage;
	
	if(0 == *count) *count = 32;	//这里最小height设置的是32
	
	return 0;
} 

/* 
 * APP调用ioctl VIDIOC_QBUF时被调用
 */
static int myvivi_buffer_prepare(struct videobuf_queue *vq, struct videobuf_buffer *vb,
						enum v4l2_field field) {
	//做些准备工作
	
	//设置状态
	vb->state = VIDEOBUF_PREPARED;
	
	return 0;
}

/*
 * APP调用ioctl VIDIOC_QBUF时: 
 * 1. 先调用buf_prepare进行一些准备工作
 * 2. 把buf放入队列
 * 3. 调用buf_queue(起通知作用)
 */
static void myvivi_buffer_queue(struct videobuf_queue *vq, struct videobuf_buffer *vb) {
	vb->state = VIDEOBUF_QUEUED;
}

//APP不使用队列时，释放内存
static void myvivi_buffer_release(struct videobuf_queue *vq, struct videobuf_buffer *vb) {
	videobuf_vmalloc_free(vb);
	vb->state = VIDEOBUF_NEEDS_INIT;
}


static struct videobuf_queue_ops myvivi_video_qops = {
	.buf_setup		= myvivi_buffer_setup,		//计算大小以免浪费
	.buf_prepare 	= myvivi_buffer_prepare,	//
	.buf_queue		= myvivi_buffer_queue,		//
	.buf_release	= myvivi_buffer_release,		//
};


static int myvivi_open(struct file *file) {
	/* 初始化队列 */
	/* ?? */
	videobuf_queue_vmalloc_init(&myvivi->vb_vidqueue, &myvivi_video_qops,
				NULL,&myvivi->queue_slock, V4L2_BUF_TYPE_VIDEO_CAPTURE,
				V4L2_FIELD_INTERLACED, sizeof(struct videobuf_buffer), NULL,NULL);
	return 0;
}

static int myvivi_close(struct file *file) {
	videobuf_stop(&myvivi->vb_vidqueue);
	videobuf_mmap_free(&myvivi->vb_vidqueue);
	
	return 0;
}

static int myvivi_mmap(struct file *file, struct vm_area_struct *vma) {
	return videobuf_mmap_mapper(&myvivi->vb_vidqueue, vma);//??
}


static const struct v4l2_file_operations myvivi_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	.open			= myvivi_open,
	.release		= myvivi_close,
	.mmap			= myvivi_mmap,
};


static void myvivi_release(struct video_device *vdev) {

}

static int myvivi_probe(struct platform_device *pdev) {
	int ret = -1;
	
	myvivi = kzalloc(sizeof(*myvivi), GFP_KERNEL);
	if (!myvivi)
		return -ENOMEM;
	
	/* 0.注册v4l2_dev */
	ret = v4l2_device_register(&pdev->dev,&myvivi->v4l2_dev);
	if (ret < 0) {
		printk(KERN_ERR"Failed to register v4l2_device: %d\n", ret);
		goto v4l2_dev_err;
	}

	/* 1.分配一个video_device */
	myvivi->vdev = video_device_alloc();
	if(IS_ERR(myvivi->vdev)) {
		printk(KERN_ERR"alloc video device error!!!\n");
		goto alloc_vdev_err;
	}

	/* 2.设置 */
	myvivi->vdev->release = myvivi_release;
	myvivi->vdev->fops = &myvivi_fops;
	myvivi->vdev->ioctl_ops = &myvivi_ioctl_ops;
	myvivi->vdev->v4l2_dev = &myvivi->v4l2_dev;
	myvivi->vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	
	//初始化一个spinlock, 初始化队列列时会用到
	spin_lock_init(&myvivi->queue_slock);

	/* 3.注册 */
	ret = video_register_device(myvivi->vdev,VFL_TYPE_GRABBER,-1);
	if(ret < 0){
		printk(KERN_ERR"register video error!!!\n");
		goto video_reg_err;
	}

	return ret;

video_reg_err:
	video_device_release(myvivi->vdev);
	
alloc_vdev_err:
	v4l2_device_put(&myvivi->v4l2_dev);
	
v4l2_dev_err:

	return -1;
}


static int myvivi_remove(struct platform_device *pdev){
	video_unregister_device(myvivi->vdev);
    video_device_release(myvivi->vdev);
	v4l2_device_put(&myvivi->v4l2_dev);
	
	return 0;
}

static struct platform_device myvivi_pdev = {
	.name			= "myvivi",
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


