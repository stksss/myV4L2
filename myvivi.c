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

static const struct v4l2_ioctl_ops myvivi_ioctl_ops = {
	//表示它是一个摄像头设备
	.vidioc_querycap = myvivi_vidoc_querycap,

	//用于列举、获取、测试、设置摄像头的数据格式
	.vidioc_enum_fmt_vid_cap 	= myvivi_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap 		= myvivi_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap 	= myvivi_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap 		= myvivi_vidioc_s_fmt_vid_cap,

//	//缓冲区操作: 申请/查询/放入/取出队列
//	.vidioc_reqbufs 			= myvivi_vidioc_reqbufs,
//	.vidioc_querybuf 			= myvivi_vidioc_querybuf,
//	.vidioc_qbuf 				= myvivi_vidioc_qbuf,
//	.vidioc_dqbuf 				= myvivi_vidioc_dqbuf,
//
//	//启动/停止
//	.vidioc_streamon 			= myvivi_vidioc_streamon,
//	.vidioc_streamoff 			= myvivi_vidioc_streamoff,
};

static const struct v4l2_file_operations myvivi_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = video_ioctl2,
	
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


