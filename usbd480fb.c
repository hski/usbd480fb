/*
 * USBD480 USB display framebuffer driver
 *
 * Copyright (C) 2008  Henri Skippari
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 USBD480-LQ043 is a 480x272 pixel display with 16 bpp RBG565 colors.

 There are also displays with other resolutions so any specific size should not be assumed.

 To use this driver you should be running firmware version 0.5 (2009/05/28) or later. 
 
 Tested with display resolutions 480x272, 640x480, 240x320, 800x256
*/

/*
TODO:
-error handling
-backlight support - separate driver?
-double buffering
-alternatively instead of continuosly updating the display wait for an update command from
 application?
-performance optimisation
-suspend/resume?

*/

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h> 
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/workqueue.h>

#define USBD480_INTEPDATASIZE 16

#define USBD480_VID	0x16C0
#define USBD480_PID	0x08A6

#define USBD480_SET_ADDRESS 0xC0
#define USBD480_SET_FRAME_START_ADDRESS 0xC4
#define USBD480_SET_BRIGHTNESS 0x81
#define USBD480_GET_DEVICE_DETAILS 0x80


#define IOCTL_SET_BRIGHTNESS 0x10
#define IOCTL_GET_DEVICE_DETAILS 0x20


#define USBD480_VIDEOMEMORDER (get_order(PAGE_ALIGN(dev->vmemsize)))

#define USBD480_REFRESH_DELAY 1000/100 /* about xx fps, less in practice */
#define USBD480_REFRESH_JIFFIES ((USBD480_REFRESH_DELAY * HZ)/1000)

#define USBD480_DEVICE(vid, pid)			\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE | 	\
		USB_DEVICE_ID_MATCH_INT_CLASS |		\
		USB_DEVICE_ID_MATCH_INT_PROTOCOL,	\
	.idVendor = (vid),				\
	.idProduct = (pid),				\
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC,	\
	.bInterfaceProtocol = 0x00

static struct usb_device_id id_table [] = {
	{ USBD480_DEVICE(USBD480_VID, USBD480_PID) },
	{ },
};
MODULE_DEVICE_TABLE (usb, id_table);

static int refresh_delay = 10;
module_param(refresh_delay, int, 0);
MODULE_PARM_DESC(refresh_delay, "Delay between display refreshes");

struct usbd480 {
	struct usb_device *udev;
	struct fb_info *fbinfo;
	struct delayed_work work;
	struct workqueue_struct *wq;

	unsigned char *vmem;
	unsigned long vmemsize;
	unsigned long vmem_phys;
	unsigned int disp_page;
	unsigned char brightness;
	unsigned int width;
	unsigned int height;
	char device_name[20];
};

static int usbd480_get_device_details(struct usbd480 *dev)
{
// TODO: return value handling

	int result;
	unsigned char *buffer;

	buffer = kmalloc(64, GFP_KERNEL);
	if (!buffer) {
		dev_err(&dev->udev->dev, "out of memory\n");
		return 0;
	}

	result = usb_control_msg(dev->udev,
				usb_rcvctrlpipe(dev->udev, 0),
				USBD480_GET_DEVICE_DETAILS,
				USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				0,
				0,
				buffer,	
				64,
				1000);
	if (result)
		dev_dbg(&dev->udev->dev, "result = %d\n", result);

	dev->width = (unsigned char)buffer[20] | ((unsigned char)buffer[21]<<8);
	dev->height = (unsigned char)buffer[22] | ((unsigned char)buffer[23]<<8);
	strncpy(dev->device_name, buffer, 20);
	kfree(buffer);	

	return 0;
}

static int usbd480_set_brightness(struct usbd480 *dev, unsigned int brightness)	
{
// TODO: return value handling, check valid dev?
	
	int result;

	result = usb_control_msg(dev->udev,
				usb_sndctrlpipe(dev->udev, 0),
				USBD480_SET_BRIGHTNESS,
				USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				brightness,
				0,
				NULL,	
				0,
				1000);
	if (result)
		dev_dbg(&dev->udev->dev, "result = %d\n", result);
						
	return 0;							
}

static int usbd480_set_address(struct usbd480 *dev, unsigned int addr)
{
// TODO: return value handling
	int result;

	result = usb_control_msg(dev->udev,
				usb_sndctrlpipe(dev->udev, 0),
				USBD480_SET_ADDRESS,
				USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				addr,
				addr>>16,
				NULL,	
				0,
				1000);
	if (result)
		dev_dbg(&dev->udev->dev, "result = %d\n", result);
						
	return 0;	
}

static int usbd480_set_frame_start_address(struct usbd480 *dev, unsigned int addr)
{
// TODO: return value handling
	int result;

	result = usb_control_msg(dev->udev,
				usb_sndctrlpipe(dev->udev, 0),
				USBD480_SET_FRAME_START_ADDRESS,
				USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
				addr,
				addr>>16,
				NULL,	
				0,
				1000);
	if (result)
		dev_dbg(&dev->udev->dev, "result = %d\n", result);
						
	return 0;	
}

static ssize_t show_brightness(struct device *dev, struct device_attribute *attr, char *buf)		
{									
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usbd480 *d = usb_get_intfdata(intf);			
									
	return sprintf(buf, "%d\n", d->brightness);			
}		
							
static ssize_t set_brightness(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)	
{								
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usbd480 *d = usb_get_intfdata(intf);			
	int brightness = simple_strtoul(buf, NULL, 10);			
									
	d->brightness = brightness;

	usbd480_set_brightness(d, brightness);	
						
	return count;
}

static ssize_t show_width(struct device *dev, struct device_attribute *attr, char *buf)		
{									
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usbd480 *d = usb_get_intfdata(intf);	
									
	return sprintf(buf, "%d\n", d->width);			
}

static ssize_t show_height(struct device *dev, struct device_attribute *attr, char *buf)		
{									
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usbd480 *d = usb_get_intfdata(intf);	
									
	return sprintf(buf, "%d\n", d->height);			
}

static ssize_t show_name(struct device *dev, struct device_attribute *attr, char *buf)		
{						
	struct usb_interface *intf = to_usb_interface(dev);		
	struct usbd480 *d = usb_get_intfdata(intf);
									
	return sprintf(buf, "%s\n", d->device_name);			
}

static DEVICE_ATTR(brightness, S_IWUGO | S_IRUGO, show_brightness, set_brightness);
static DEVICE_ATTR(width, S_IRUGO, show_width, NULL);
static DEVICE_ATTR(height, S_IRUGO, show_height, NULL);
static DEVICE_ATTR(name, S_IRUGO, show_name, NULL);

static void usbd480fb_work(struct work_struct *work)
{
	struct usbd480 *d =
		container_of(work, struct usbd480, work.work);
	int result;	
	int sentsize;
	int writeaddr;
	int showaddr;

	if(d->disp_page == 0)
	{
		writeaddr = 0;
		showaddr = 0;
		d->disp_page = 1;
	}
	else
	{	
		writeaddr = d->vmemsize;
		showaddr = d->vmemsize;
		d->disp_page = 0;
	}

	usbd480_set_address(d, writeaddr);

	result = usb_bulk_msg(d->udev,
				usb_sndbulkpipe(d->udev, 2),
				d->vmem,
				d->vmemsize,
				&sentsize, 5000);

	usbd480_set_frame_start_address(d, showaddr);

	queue_delayed_work(d->wq, &d->work, USBD480_REFRESH_JIFFIES);
}


/*
static long usbd480_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

	return 0;
}

static struct file_operations usbd480_fops = {
	.owner =	THIS_MODULE,
//	.read =		usbd480_read,
//	.write =	usbd480_write,
	.unlocked_ioctl = usbd480_ioctl,
//	.open =		usbd480_open,
//	.release =	usbd480_release,
};

static struct usb_class_driver usbd480_class = {
	.name =		"usbd480",
	.fops =		&usbd480_fops,
	.minor_base =	USBD480_MINOR_BASE,
};
*/


#ifdef USBD480FB_PAN
static int usbd480fb_pan_display(struct fb_var_screeninfo *var,
			struct fb_info *info)
{
    struct usbd480 *pusbd480fb = info->par;
    unsigned int framestart;
    int result;

    if (var->xoffset != 0) /* not supported */
	return -EINVAL;

    if (var->yoffset + info->var.yres > info->var.yres_virtual)
	return -EINVAL;
    
    framestart = (info->fix.line_length >> 1) * var->yoffset;

    result = usb_control_msg(pusbd480fb->udev,
		usb_sndctrlpipe(pusbd480fb->udev, 0),
		USBD480_SET_FRAME_START_ADDRESS,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_INTERFACE,
		framestart,
		framestart>>16,
		NULL,	
		0,
		1000);
    
    return 0;    
}
#endif

static struct fb_ops usbd480fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= fb_sys_write,
	.fb_fillrect	= sys_fillrect, 
	.fb_copyarea	= sys_copyarea,	
	.fb_imageblit	= sys_imageblit,
	//.fb_ioctl	= usbd480fb_ioctl,
	//.fb_mmap	= usbd480fb_mmap,
	//.fb_pan_display = usbd480fb_pan_display,
};

static int usbd480_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usbd480 *dev = NULL;
	int retval = -ENOMEM;
	signed long size;
	unsigned long addr;
	struct fb_info *info;

	dev = kzalloc(sizeof(struct usbd480), GFP_KERNEL);
	if (dev == NULL) {
		retval = -ENOMEM;
		dev_err(&interface->dev, "Out of memory\n");
		goto error_dev;
	}

	dev->udev = usb_get_dev(udev);
	usb_set_intfdata (interface, dev);

/*
	retval = usb_register_dev(interface, &usbd480_class);
	if (retval) {
		err("Not able to get a minor for this device.");
		return -ENOMEM;
	}
*/

	retval = device_create_file(&interface->dev, &dev_attr_brightness);
	if (retval)
		goto error_dev_attr;
	retval = device_create_file(&interface->dev, &dev_attr_width);
	if (retval)
		goto error_dev_attr;
	retval = device_create_file(&interface->dev, &dev_attr_height);
	if (retval)
		goto error_dev_attr;
	retval = device_create_file(&interface->dev, &dev_attr_name);
	if (retval)
		goto error_dev_attr;

	dev_info(&interface->dev, "USBD480 attached\n");
	//printk(KERN_INFO "usbd480fb: USBD480 connected\n");

	usbd480_get_device_details(dev);
	dev->vmemsize = dev->width*dev->height*2;
	dev->vmem = NULL;

	if (!(dev->vmem = (void *)__get_free_pages(GFP_KERNEL, USBD480_VIDEOMEMORDER))) {
		printk(KERN_ERR ": can't allocate vmem buffer");
		retval = -ENOMEM;
		goto error_vmem;
	}

	size = PAGE_SIZE * (1 << USBD480_VIDEOMEMORDER);
	addr = (unsigned long)dev->vmem;

	while (size > 0) {
	    SetPageReserved(virt_to_page((void*)addr));
	    addr += PAGE_SIZE;
	    size -= PAGE_SIZE;
	}

	dev->vmem_phys = virt_to_phys(dev->vmem);
	memset(dev->vmem, 0, dev->vmemsize);

	info = framebuffer_alloc(0, NULL);
	if (!info)
	{
		printk("error: framebuffer_alloc\n");
		goto error_fballoc;
	}

	info->screen_base = (char __iomem *) dev->vmem;
	info->screen_size = dev->vmemsize;
	info->fbops = &usbd480fb_ops;

	info->fix.type =	FB_TYPE_PACKED_PIXELS;
	info->fix.visual =	FB_VISUAL_TRUECOLOR;
	info->fix.xpanstep =	0;
	info->fix.ypanstep =	0; /*1*/
	info->fix.ywrapstep =	0; 
	info->fix.line_length = dev->width*16/8;
	info->fix.accel =	FB_ACCEL_NONE;

	info->fix.smem_start  = (unsigned long)dev->vmem_phys;
	info->fix.smem_len = dev->vmemsize;

	info->var.xres = 		dev->width;
	info->var.yres = 		dev->height;
	info->var.xres_virtual = 	dev->width;
	info->var.yres_virtual = 	dev->height; /*8738*/
	info->var.bits_per_pixel = 	16;
	info->var.red.offset = 		11;	
	info->var.red.length = 		5;
      	info->var.green.offset = 	5;	
	info->var.green.length = 	6;
      	info->var.blue.offset =		0;
      	info->var.blue.length = 	5;
      	info->var.left_margin =		0;
      	info->var.right_margin =	0;
      	info->var.upper_margin =	0;
      	info->var.lower_margin =	0;	
      	info->var.vmode =		FB_VMODE_NONINTERLACED;

	info->pseudo_palette = NULL;
	info->par = NULL;
	info->flags = FBINFO_FLAG_DEFAULT; /*FBINFO_HWACCEL_YPAN */

	info->pseudo_palette = kzalloc(sizeof(u32)*16, GFP_KERNEL);
	if (info->pseudo_palette == NULL) {
		retval = -ENOMEM;
		dev_err(&interface->dev, "Failed to allocate pseudo_palette\n");
		goto error_fbpseudopal;
	}
	
	memset(info->pseudo_palette, 0, sizeof(u32)*16);
	
	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0) {
		retval = -ENOMEM;
		dev_err(&interface->dev, "Failed to allocate cmap\n");
		goto error_fballoccmap;	
	}

	//printk("phys=%lx p2v=%lx, v=%lx\n", dev->vmem_phys, phys_to_virt(dev->vmem_phys), dev->vmem);

	retval = register_framebuffer(info);
	if (retval < 0) {
		printk("error: register_framebuffer \n");
		goto error_fbreg;
	}

	dev->fbinfo = info;
	dev->disp_page = 0;

	dev->wq = create_singlethread_workqueue("usbd480fb"); //TODO: create unique names?
	if (!dev->wq) {
		err("Could not create work queue\n");
		retval = -ENOMEM;
		goto error_wq;
	}

	INIT_DELAYED_WORK(&dev->work, usbd480fb_work);
	queue_delayed_work(dev->wq, &dev->work, USBD480_REFRESH_JIFFIES*4);

	printk(KERN_INFO
	       "fb%d: USBD480 framebuffer device, using %ldK of memory\n",
	       info->node, dev->vmemsize >> 10);

	return 0;

error_wq:
	unregister_framebuffer(info);
error_fbreg:
	fb_dealloc_cmap(&info->cmap);
error_fballoccmap:	
	if (info->pseudo_palette)
		kfree(info->pseudo_palette);
error_fbpseudopal:	
	framebuffer_release(info);		
error_fballoc:
	size = PAGE_SIZE * (1 << USBD480_VIDEOMEMORDER);
	addr = (unsigned long)dev->vmem;
	while (size > 0) {
	    ClearPageReserved(virt_to_page((void*)addr));
	    addr += PAGE_SIZE;
	    size -= PAGE_SIZE;
	}
	free_pages((unsigned long)dev->vmem, USBD480_VIDEOMEMORDER);
error_vmem:
error_dev_attr:
	device_remove_file(&interface->dev, &dev_attr_brightness);
	device_remove_file(&interface->dev, &dev_attr_width);
	device_remove_file(&interface->dev, &dev_attr_height);
	device_remove_file(&interface->dev, &dev_attr_name);
error_dev:
	usb_set_intfdata(interface, NULL);
	if (dev)
		kfree(dev);

	printk(KERN_INFO "usbd480fb: error probe\n");
	return retval;
}

static void usbd480_disconnect(struct usb_interface *interface)
{
	struct usbd480 *dev;
	struct fb_info *info;
	signed long size;
	unsigned long addr;

	dev = usb_get_intfdata (interface);

	cancel_delayed_work_sync(&dev->work);
	flush_workqueue(dev->wq);
	destroy_workqueue(dev->wq);

	//usb_deregister_dev(interface, &usbd480_class);

	device_remove_file(&interface->dev, &dev_attr_brightness);
	device_remove_file(&interface->dev, &dev_attr_width);
	device_remove_file(&interface->dev, &dev_attr_height);
	device_remove_file(&interface->dev, &dev_attr_name);

	info = dev->fbinfo;
	if (info) {
		unregister_framebuffer(info);
		framebuffer_release(info);
		size = PAGE_SIZE * (1 << USBD480_VIDEOMEMORDER);
		addr = (unsigned long)dev->vmem;
		while (size > 0) {
			ClearPageReserved(virt_to_page((void*)addr));
			addr += PAGE_SIZE;
			size -= PAGE_SIZE;
		}
		free_pages((unsigned long)dev->vmem, USBD480_VIDEOMEMORDER);
	}

	usb_set_intfdata(interface, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
	dev_info(&interface->dev, "USBD480 disconnected\n");
	
//printk(KERN_INFO "usbd480fb: USBD480 disconnected\n");
}

static struct usb_driver usbd480_driver = {
	.name =		"usbd480fb",
	.probe =	usbd480_probe,
	.disconnect =	usbd480_disconnect,
	.id_table =	id_table,
};

static int __init usbd480_init(void)
{
	int retval = 0;

	retval = usb_register(&usbd480_driver);
	if (retval)
		err("usb_register failed. Error number %d", retval);
	return retval;
}

static void __exit usbd480_exit(void)
{
	usb_deregister(&usbd480_driver);
}

module_init (usbd480_init);
module_exit (usbd480_exit);

MODULE_AUTHOR("Henri Skippari");
MODULE_DESCRIPTION("USBD480 framebuffer driver");
MODULE_LICENSE("GPL");
