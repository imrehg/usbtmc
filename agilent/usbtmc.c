// usbtmc.c
// Linux kernel module for USBTMC (USB Test and Measurement Class) devices
// Copyright (C) 2007 Stefan Kopp, Gechingen, Germany
// See revision history at the end of this file
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// The GNU General Public License is available at
// http://www.gnu.org/copyleft/gpl.html.

// #define USBTMC_DEBUG
// If defined: causes the driver to log status messages in the kernel log

#define USBTMC_VERSION				110
// Integer representation of version code (1.1)

#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include "usbtmc.h"

static struct usb_device_id usbtmc_devices[] = {
	{.match_flags=USB_DEVICE_ID_MATCH_INT_CLASS |
		USB_DEVICE_ID_MATCH_INT_SUBCLASS,
		// Device class and sub class need to match for notification by the
		// usb core layer.
		.bInterfaceClass=254, // 254 = application specific
		.bInterfaceSubClass=3}, // 3 = test and measurement class (USBTMC)
	{ } // Empty (terminating) entry
};
// This list defines which devices are serviced by this driver. This driver
// handles USBTMC devices, so we look for the corresponding class (application
// specific) and subclass (USBTMC).

static dev_t dev;
// Will hold base (first) major/minor number to be used.
// The major number used is allocated dynamically in usbtmc_init.

static struct cdev cdev;
// Character device structure for minor number 0 (for communication with
// the driver itself, not an instrument.

static u8 usbtmc_minors[USBTMC_MINOR_NUMBERS];
// This array is used to track the status of the minor numbers allocated by
// the driver (used or unused).
// 0=unused; 1=used.
// Minor number 0 is reserved for communication with the driver itself.

static struct usbtmc_device_data *usbtmc_devs[USBTMC_MINOR_NUMBERS];
// This array will hold the private data pointers of the instruments. It is
// used by the minor 0 driver to get access to the USB sessions to retrieve
// instrument information.

struct usbtmc_device_data {
	struct cdev cdev;
	int devno;
	struct usb_interface *intf;
	const struct usb_device_id *id;
	unsigned int bulk_in;
	unsigned int bulk_out;
	u8 bTag;
	struct usb_device *usb_dev;
	u8 eof;
	char __user *retry_buf;
	size_t retry_count;
	int timeout;
	u8 term_char_enabled;
	u8 term_char;
	int fread;
	int auto_abort;
	int add_nl_on_read;
	int rem_nl_on_write;
};
// This structure holds private data for each USBTMC device. One copy is
// allocated for each USBTMC device in the driver's probe function.

static struct usb_driver usbtmc_driver;
// This structure contains registration information for the driver. The
// information is passed to the system through usb_register(), called in the
// driver's init function.

static u8 *usbtmc_buffer;
// Pointer to buffer for I/O data (allocated in usbtmc_init)


static u8 usbtmc_last_write_bTag;
static u8 usbtmc_last_read_bTag;
// Last bTag values (needed for abort)

// static struct completion usbtmc_init_done;
// "Completion" for driver initialization....

// Forward declarations
int usbtmc_ioctl_abort_bulk_in (struct inode *,struct file *,unsigned int,
	unsigned long);
int usbtmc_ioctl_abort_bulk_out (struct inode *,struct file *,unsigned int,
	unsigned long);

int usbtmc_open(struct inode *inode,struct file *filp)
// This function is called when opening an instrument device file. It looks for
// the device's USB endpoints for later access.
{
	struct usbtmc_device_data *p_device_data;
	u8 n;
	u8 bulk_in,bulk_out;
	struct usb_host_interface *curr_setting;
	struct usb_endpoint_descriptor *end_point;

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_open called\n");
	#endif
	
	if(!iminor(inode)) goto minor_null; // Driver communication

	// Get pointer to instrument's private data
	p_device_data = container_of(inode->i_cdev,struct usbtmc_device_data,cdev);

	// Store pointer in file structure's private data field
	filp->private_data=p_device_data;

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Number of USB settings is %u\n",
		p_device_data->intf->num_altsetting);
	#endif

	// USBTMC devices have only one setting, so use that (current setting)
	curr_setting=p_device_data->intf->cur_altsetting;
	
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Number of endpoints is %u\n",
		curr_setting->desc.bNumEndpoints);
	#endif

	// Find bulk in endpoint
	bulk_in=0;
	for(n=0;n<(curr_setting->desc.bNumEndpoints);n++) {
		end_point=&(curr_setting->endpoint[n].desc);
		if((end_point->bEndpointAddress & USB_DIR_IN) &&
			((end_point->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)==
			USB_ENDPOINT_XFER_BULK)) {
			bulk_in=end_point->bEndpointAddress;
			#ifdef USBTMC_DEBUG	
			printk(KERN_NOTICE "USBTMC: Found bulk in endpoint at %u\n",
				bulk_in);
			#endif
			n=curr_setting->desc.bNumEndpoints; // Exit loop
		}
	}
	p_device_data->bulk_in=bulk_in;

	// Find bulk out endpoint
	bulk_out=0;
	for(n=0;n<(curr_setting->desc.bNumEndpoints);n++) {
		end_point=&(curr_setting->endpoint[n].desc);
		if(!(end_point->bEndpointAddress & USB_DIR_IN) &&
			((end_point->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)==
			USB_ENDPOINT_XFER_BULK)) {
			bulk_out=end_point->bEndpointAddress;
			#ifdef USBTMC_DEBUG		
			printk(KERN_NOTICE "USBTMC: Found Bulk out endpoint at %u\n",
				bulk_out);
			#endif	
			n=curr_setting->desc.bNumEndpoints; // Exit loop
		}
	}
	p_device_data->bulk_out=bulk_out;
	
	return 0;

minor_null:
	// Minor number 0 is reserved for communication with driver
	
	// Allocate memory for private data
	if(!(p_device_data=kmalloc(sizeof(struct usbtmc_device_data),GFP_KERNEL))) {
		printk(KERN_ALERT "USBTMC: Unable to allocate kernel memory\n");
		return -ENOMEM;
	}
	
	// Set major/minor to 0
	p_device_data->devno=0;
	
	// Store pointer in file structure's private data field
	filp->private_data=p_device_data;
	
	return 0;	
}

int usbtmc_release(struct inode *inode,struct file *filp)
// This function is called when closing the instrument device file.
{
	#ifdef USBTMC_DEBUG
	printk(KERN_ALERT "USBTMC: usbtmc_release called\n");
	#endif
	
	if(!iminor(inode)) goto minor_null; // Driver communication

	return 0;
	
minor_null:
	// Minor number 0 is reserved for communication with driver
	
	// Free buffer allocated in usbtmc_init
	kfree(filp->private_data);
	
	return 0;
}

int usbtmc_buffer_append(char *dest,char *src,int *count)
// Helper function for usbtmc_read.
{
	int n=0;
	while(*src!=0){ // Continue to the end of source string
		*dest=*src;
		dest++;
		src++;
		n++;
		(*count)++;
	}
	return n;
}

ssize_t usbtmc_read(struct file *filp,
	char __user *buf,
	size_t count,
	loff_t *f_pos)
// This function reads the instrument's output buffer through a
// USMTMC DEV_DEP_MSG_IN message.
{
	struct usbtmc_device_data *p_device_data;
	unsigned int pipe;
	int retval;
	int actual;
	unsigned long int n_characters;
	struct usb_device *p_device;
	int n;
	char *p;
	char *s;
	int t;
	int remaining;
	int done;
	int this_part;
	
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_read called\n");
	printk(KERN_NOTICE "USBTMC: Count is %u\n",count);
	#endif
		
	// Get pointer to private data structure
	p_device_data=filp->private_data;
	
	if(!p_device_data->devno) goto minor_null; // Driver communication
	
	if((p_device_data->fread)&&(p_device_data->eof)) {
		// Zero will tell fread that the EOF was reached. It will keep fread
		// from retrying to read up to the max number of characters.
		p_device_data->eof=0;
		return 0;
	}
	
	remaining=count;
	done=0;
	
	while(remaining>0)
	{
		if(remaining>USBTMC_SIZE_IOBUFFER-12-3) {
			this_part=USBTMC_SIZE_IOBUFFER-12-3;
		}
		else {
			this_part=remaining;
		}
		
		// Setup IO buffer for DEV_DEP_MSG_IN message
		usbtmc_buffer[0]=2; // REQUEST_DEV_DEP_MSG_IN
		usbtmc_buffer[1]=p_device_data->bTag; // Transfer ID (bTag)
		usbtmc_buffer[2]=~(p_device_data->bTag); // Inverse of bTag
		usbtmc_buffer[3]=0; // Reserved
		usbtmc_buffer[4]=(this_part-12-3)&255; // Max transfer (first byte)
		usbtmc_buffer[5]=((this_part-12-3)>>8)&255; // Second byte
		usbtmc_buffer[6]=((this_part-12-3)>>16)&255; // Third byte
		usbtmc_buffer[7]=((this_part-12-3)>>24)&255; // Fourth byte
		usbtmc_buffer[8]=p_device_data->term_char_enabled*2;
		// Use term character?
		usbtmc_buffer[9]=p_device_data->term_char; // Term character
		usbtmc_buffer[10]=0; // Reserved
		usbtmc_buffer[11]=0; // Reserved
	
		// Create pipe for bulk out transfer	
		pipe=usb_sndbulkpipe(p_device_data->usb_dev,p_device_data->bulk_out);
	
		// Send bulk URB
		retval=usb_bulk_msg(p_device_data->usb_dev,pipe,usbtmc_buffer,12,
			&actual,p_device_data->timeout);
			
		// Store bTag (in case we need to abort)
		usbtmc_last_write_bTag=p_device_data->bTag;
	
		// Increment bTag -- and increment again if zero
		(p_device_data->bTag)++;
		if(!(p_device_data->bTag)) (p_device_data->bTag)++;
		
		if(retval<0) {
			printk(KERN_NOTICE "USBTMC: usb_bulk_msg returned %d\n",
				retval);
			if(p_device_data->auto_abort)
				usbtmc_ioctl_abort_bulk_out(0,filp,USBTMC_IOCTL_ABORT_BULK_OUT,
					0);
			return retval;
		}
	
		// Create pipe for bulk in transfer	
		pipe=usb_rcvbulkpipe(p_device_data->usb_dev,p_device_data->bulk_in);
	
		// Send bulk URB
		retval=usb_bulk_msg(p_device_data->usb_dev,pipe,usbtmc_buffer,
			USBTMC_SIZE_IOBUFFER,&actual,p_device_data->timeout);
		
		// Store bTag (in case we need to abort)
		usbtmc_last_read_bTag=p_device_data->bTag;
	
		if(retval<0) {
			printk(KERN_NOTICE "USBTMC: Unable to read data, error %d\n",
				retval);
			if(p_device_data->auto_abort)
				usbtmc_ioctl_abort_bulk_in(0,filp,USBTMC_IOCTL_ABORT_BULK_IN,0);
			return retval;
		}
	
		// How many characters did the instrument send?
		n_characters=usbtmc_buffer[4]+
			(usbtmc_buffer[5]<<8)+
			(usbtmc_buffer[6]<<16)+
			(usbtmc_buffer[7]<<24);
	
		// Copy buffer to user space
		if(copy_to_user(buf+done,&usbtmc_buffer[12],n_characters)) {
			// There must have been an addressing problem
			return -EFAULT;
		}
		
		done+=n_characters;
		if(n_characters<USBTMC_SIZE_IOBUFFER) remaining=0;
	}
	
	if(p_device_data->add_nl_on_read==USBTMC_ATTRIB_VAL_ON) {
		// Add newline character
		if(done<count) { // Still space in user buffer?
			usbtmc_buffer[0]='\n';
			if(copy_to_user(buf+done,&usbtmc_buffer[0],1)) {
				// There must have been an addressing problem
				return -EFAULT;
			}
			done+=1;
		}
	}
	
	// Update file position value
	*f_pos=*f_pos+done;
	
	// If less bytes than the requested number are returned, fread will
	// retry. Make sure the next read (retry) returns 0 -- this will cause
	// fread to give up (EOF).
	if(done<count) p_device_data->eof=1;
	
	return done; // Number of bytes read (total)
	
minor_null:
	// Minor number 0 is reserved for communication with this driver.
	// Reading from this minor number returns a list of the devices currently
	// registered. This allows you to identify the minor number to use for a
	// given device.
	
	// Return 0 (end of file) if not reading from beginning (f_pos==0).
	// This will keep fread from trying to read the full number of requested
	// bytes. You must read the whole data with the initial call because
	// subsequent calls will return "end of file".
	if(*f_pos>0) return 0;
	
	p=(char *)usbtmc_buffer;
	t=0;
	
	// Add header line to buffer
	p=p+usbtmc_buffer_append(p,
		"Minor Number\tManufacturer\tProduct\tSerial Number\n",&t);
	
	// Find out which minor numbers are used
	for(n=1;n<USBTMC_MINOR_NUMBERS;n++)
		if(usbtmc_minors[n]) {
		// Add entry for this device
		p_device=interface_to_usbdev(usbtmc_devs[n]->intf);
		sprintf(p,"%03d\t",n); p=p+4; t=t+4;
		s=p_device->manufacturer;
		p=p+usbtmc_buffer_append(p,s,&t);
		p=p+usbtmc_buffer_append(p,"\t",&t);
		s=p_device->product;
		p=p+usbtmc_buffer_append(p,s,&t);
		p=p+usbtmc_buffer_append(p,"\t",&t);
		s=p_device->serial;
		p=p+usbtmc_buffer_append(p,s,&t);
		p=p+usbtmc_buffer_append(p,"\n",&t);
	}

	// Copy buffer to user space
	if(copy_to_user(buf,usbtmc_buffer,t)) {
		// There must have been an addressing problem
		return -EFAULT;
	}
	
	// Update file position value
	*f_pos=*f_pos+t;
	
	return t; // Number of bytes read
}

// This function sends a command to an instrument by wrapping it into a
// USMTMC DEV_DEP_MSG_OUT message.
ssize_t usbtmc_write(struct file *filp,
	const char __user *buf,
	size_t count,
	loff_t *f_pos)
{
	struct usbtmc_device_data *p_device_data;
	unsigned int pipe;
	int retval;
	int actual;
	unsigned long int n_bytes;
	int n;
	int remaining;
	int done;
	int this_part;

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_write called\n");
	#endif
	
	// Get pointer to private data structure
	p_device_data=filp->private_data;
	
	if(!p_device_data->devno) goto minor_null; // Driver communication
	
	p_device_data->eof=0;
	
	remaining=count;
	done=0;
	
	while(remaining>0) // Still bytes to send
	{
		if(remaining>USBTMC_SIZE_IOBUFFER-12) {
			// Use maximum size (limited by driver internal buffer size)
			this_part=USBTMC_SIZE_IOBUFFER-12; // Use maximum size
			usbtmc_buffer[8]=0; // This is not the last transfer -- see below
		}
		else {
			// Can send remaining bytes in a single transaction
			this_part=remaining;
			usbtmc_buffer[8]=1; // Message ends with this transfer -- see below
		}
		
		// Setup IO buffer for DEV_DEP_MSG_OUT message
		usbtmc_buffer[0]=1; // DEV_DEP_MSG_OUT
		usbtmc_buffer[1]=p_device_data->bTag; // Transfer ID (bTag)
		usbtmc_buffer[2]=~(p_device_data->bTag); // Inverse of bTag
		usbtmc_buffer[3]=0; // Reserved
		usbtmc_buffer[4]=this_part&255; // Transfer size (first byte)
		usbtmc_buffer[5]=(this_part>>8)&255; // Transfer size (second byte)
		usbtmc_buffer[6]=(this_part>>16)&255; // Transfer size (third byte)
		usbtmc_buffer[7]=(this_part>>24)&255; // Transfer size (fourth byte)
		// usbtmc_buffer[8] is set above...
		usbtmc_buffer[9]=0; // Reserved
		usbtmc_buffer[10]=0; // Reserved
		usbtmc_buffer[11]=0; // Reserved
		
		// Append write buffer (instrument command) to USBTMC message
		if(copy_from_user(&(usbtmc_buffer[12]),buf+done,this_part)) {
			// There must have been an addressing problem
			return -EFAULT;
		}	
		
		if(this_part==remaining) { // If this is the last transfer...
			if(p_device_data->rem_nl_on_write==USBTMC_ATTRIB_VAL_ON) {
				// See if last byte to send is a '\n'...
				if(usbtmc_buffer[12+this_part-1]=='\n') {
					// Remove it!
					this_part--;
				}
			}
		}
	
		// Add zero bytes to achieve 4-byte alignment
		n_bytes=12+this_part;
		if(this_part%4) {
			n_bytes+=4-this_part%4;
			for(n=12+this_part;n<n_bytes;n++) usbtmc_buffer[n]=0;
		}
	
		// Create pipe for bulk out transfer	
		pipe=usb_sndbulkpipe(p_device_data->usb_dev,p_device_data->bulk_out);
	
		// Send bulk URB
		retval=usb_bulk_msg(p_device_data->usb_dev,pipe,usbtmc_buffer,n_bytes,
			&actual,p_device_data->timeout);
	
		// Store bTag (in case we need to abort)
		usbtmc_last_write_bTag=p_device_data->bTag;
		
		// Increment bTag -- and increment again if zero
		(p_device_data->bTag)++;
		if(!(p_device_data->bTag)) (p_device_data->bTag)++;
		
		if(retval<0) {
			printk(KERN_NOTICE "USBTMC: Unable to send data, error %d\n",
				retval);
			if(p_device_data->auto_abort)
				usbtmc_ioctl_abort_bulk_out(0,filp,USBTMC_IOCTL_ABORT_BULK_OUT,
					0);
			return retval;
		}
		
		remaining-=this_part;
		done+=this_part;
	}
	
	return count;
	
minor_null:
	// Minor number 0 is reserved for communication with driver
	
	return -EPERM;
}

int usbtmc_ioctl_abort_bulk_in (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Abort the last bulk in transfer and restore synchronization.
// See section 4.2.1.4 of the USBTMC specifcation for details.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	int n;
	int actual;
	struct usb_host_interface *current_setting;
	int max_size;

	// Get pointer to private data structure
	p_device_data=filp->private_data;
	
	// INITIATE_ABORT_BULK_IN request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_INITIATE_ABORT_BULK_IN, // INITIATE_ABORT
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_ENDPOINT, // Request type
		usbtmc_last_read_bTag, // Last transaction's bTag value
		p_device_data->bulk_in, // Endpoint
		usbtmc_buffer, // Target buffer
		2, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
				
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: INITIATE_ABORT_BULK_IN returned %x\n",
		usbtmc_buffer[0]);
	#endif
	
	if(usbtmc_buffer[0]==USBTMC_STATUS_FAILED) {
		// No transfer in progress and bulk in fifo is empty.
		return 0;
	}
		
	if(usbtmc_buffer[0]!=USBTMC_STATUS_SUCCESS)
	{
		printk(KERN_NOTICE
			"USBTMC: INITIATE_ABORT_BULK_IN returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}
			
	// Get wMaxPacketSize
	max_size=0;
	current_setting=p_device_data->intf->cur_altsetting;
	// Find data structure for bulk in endpoint
	for(n=0;n<current_setting->desc.bNumEndpoints;n++)
		if(current_setting->endpoint[n].desc.bEndpointAddress==
			p_device_data->bulk_in)
			// Now get that endpoint's wMaxPacketSize
			max_size=current_setting->endpoint[n].desc.wMaxPacketSize;
	if(max_size==0) {
		printk(KERN_NOTICE "USBTMC: Couldn't get wMaxPacketSize\n");
		return -EPERM;
	}
			
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: wMaxPacketSize is %d\n",max_size);
	#endif
			
	n=0; // To keep track of the number of read operations (see below)
			
	do {
		// Read a chunk of data from bulk in endpoint
				
		#ifdef USBTMC_DEBUG
		printk(KERN_NOTICE "USBTMC: Reading from bulk in EP\n");
		#endif
						
		// Read from bulk in EP
		rv=usb_bulk_msg(p_device_data->usb_dev,
			usb_rcvbulkpipe(p_device_data->usb_dev,
				p_device_data->bulk_in), // Bulk in pipe
			usbtmc_buffer, // Target buffer
			USBTMC_SIZE_IOBUFFER, // Max characters to read
			&actual, // Actual characters read
			p_device_data->timeout); // Timeout (jiffies)
				
		n++;
				
		if(rv<0) { // I/O error
			printk(KERN_NOTICE "USBTMC: usb_bulk_msg returned %d\n",
				rv);
			return rv;
		}
	} while ((actual==max_size)&&(n<USBTMC_MAX_READS_TO_CLEAR_BULK_IN));
			
	if(actual==max_size) { // Couldn't clear device buffer
		printk(KERN_NOTICE
			"USBTMC: Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		return -EPERM;
	}
			
	n=0; // To keep track of the number of read operations (see below)
			
usbtmc_abort_bulk_in_status:			
			
	// CHECK_ABORT_BULK_IN_STATUS request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_CHECK_ABORT_BULK_IN_STATUS, // CHECK_STATUS
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_ENDPOINT, // Request type
		0, // Reserved
		p_device_data->bulk_in, // Endpoint
		usbtmc_buffer, // Target buffer
		0x08, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
				
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: INITIATE_ABORT_BULK_IN returned %x\n",
		usbtmc_buffer[0]);
	#endif
			
	if(usbtmc_buffer[0]==USBTMC_STATUS_SUCCESS)
		return 0;
			
	if(usbtmc_buffer[0]!=USBTMC_STATUS_PENDING) {
		printk(KERN_NOTICE
			"USBTMC: INITIATE_ABORT_BULK_IN returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}
	
	// Is there data to read off the device?
	if(usbtmc_buffer[1]==1) do {
		// Read a chunk of data from bulk in endpoint
				
		#ifdef USBTMC_DEBUG
		printk(KERN_NOTICE "USBTMC: Reading from bulk in EP\n");
		#endif
						
		// Read from bulk in EP
		rv=usb_bulk_msg(p_device_data->usb_dev,
			usb_rcvbulkpipe(p_device_data->usb_dev,
				p_device_data->bulk_in), // Bulk in pipe
			usbtmc_buffer, // Target buffer
			USBTMC_SIZE_IOBUFFER, // Max characters to read
			&actual, // Actual characters read
			p_device_data->timeout); // Timeout (jiffies)
				
		n++;
				
		if(rv<0) { // I/O error
			printk(KERN_NOTICE "USBTMC: usb_bulk_msg returned %d\n",
				rv);
			return rv;
		}
	} while ((actual=max_size)&&(n<USBTMC_MAX_READS_TO_CLEAR_BULK_IN));	
				
	if(actual==max_size) { // Couldn't clear device buffer
		printk(KERN_NOTICE
			"USBTMC: Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		return -EPERM;
	}

	// Device should be clear at this point. Now check status again!
	goto usbtmc_abort_bulk_in_status;	
}

int usbtmc_ioctl_abort_bulk_out (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Abort the last bulk out transfer and restore synchronization.
// See section 4.2.1.2 of the USBTMC specifcation for details.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	int n;

	// Get pointer to private data structure
	p_device_data=filp->private_data;		
			
	// INITIATE_ABORT_BULK_OUT request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_INITIATE_ABORT_BULK_OUT, // INITIATE_ABORT
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_ENDPOINT, // Request type
		usbtmc_last_write_bTag, // Last transaction's bTag value
		p_device_data->bulk_out, // Endpoint
		usbtmc_buffer, // Target buffer
		2, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
				
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: INITIATE_ABORT_BULK_OUT returned %x\n",
		usbtmc_buffer[0]);
	#endif
		
	if(usbtmc_buffer[0]!=USBTMC_STATUS_SUCCESS)
	{
		printk(KERN_NOTICE
			"USBTMC: INITIATE_ABORT_BULK_OUT returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}
			
	n=0; // To keep track of the number of turns (see below)
			
usbtmc_abort_bulk_out_check_status:
			
	// CHECK_ABORT_BULK_OUT request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_CHECK_ABORT_BULK_OUT_STATUS, // CHECK_STATUS
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_ENDPOINT, // Request type
		0, // Reserved
		p_device_data->bulk_out, // Endpoint
		usbtmc_buffer, // Target buffer
		0x08, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	n++;
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
				
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: CHECK_ABORT_BULK_OUT returned %x\n",
		usbtmc_buffer[0]);
	#endif
			
	if(usbtmc_buffer[0]==USBTMC_STATUS_SUCCESS)
		goto usbtmc_abort_bulk_out_clear_halt;
		
	if((usbtmc_buffer[0]==USBTMC_STATUS_PENDING)&&
		(n<USBTMC_MAX_READS_TO_CLEAR_BULK_IN))
		goto usbtmc_abort_bulk_out_check_status;
			
	printk(KERN_NOTICE "USBTMC: CHECK_ABORT_BULK_OUT returned %x\n",
		usbtmc_buffer[0]);
	return -EPERM;
			
usbtmc_abort_bulk_out_clear_halt:
		
	// CLEAR_FEATURE request to clear bulk out halt
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_sndctrlpipe(p_device_data->usb_dev,0), // Control out pipe
		USB_REQ_CLEAR_FEATURE, // Clear feature request
		USB_DIR_OUT|USB_TYPE_STANDARD|USB_RECIP_ENDPOINT, // To EP
		USB_ENDPOINT_HALT, // Feature ENDPOINT_HALT
		p_device_data->bulk_out,
		usbtmc_buffer, // Target buffer
		0, // Although we don't really want data this time
		p_device_data->timeout);
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
			
	return 0;	
}

int usbtmc_ioctl_clear (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Clear the device's input and output buffers.
// See section 4.2.1.6 of the USBTMC specification for details.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	int n;
	int actual;
	struct usb_host_interface *current_setting;
	int max_size;

	// Get pointer to private data structure
	p_device_data=filp->private_data;

			
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Sending INITIATE_CLEAR request\n");
	#endif
		
	// INITIATE_CLEAR request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_INITIATE_CLEAR, // INITIATE_CLEAR
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE, // Request type
		0, // Interface number (always zero for USBTMC)
		0, // Reserved
		usbtmc_buffer, // Target buffer
		1, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
				
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: INITIATE_CLEAR returned %x\n",
		usbtmc_buffer[0]);
	#endif
			
	if(usbtmc_buffer[0]!=USBTMC_STATUS_SUCCESS) {
		printk(KERN_NOTICE "USBTMC: INITIATE_CLEAR returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}

	// Get wMaxPacketSize
	max_size=0;
	current_setting=p_device_data->intf->cur_altsetting;
	// Find data structure for bulk in endpoint
	for(n=0;n<current_setting->desc.bNumEndpoints;n++)
		if(current_setting->endpoint[n].desc.bEndpointAddress==
			p_device_data->bulk_in)
			// Now get that endpoint's wMaxPacketSize
			max_size=current_setting->endpoint[n].desc.wMaxPacketSize;
	if(max_size==0) {
		printk(KERN_NOTICE "USBTMC: Couldn't get wMaxPacketSize\n");
		return -EPERM;
	}
			
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: wMaxPacketSize is %d\n",max_size);
	#endif
	
	n=0; // To keep track of the number of read operations (see below)
			
usbtmc_clear_check_status:			
			
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Sending CHECK_CLEAR_STATUS request\n");
	#endif
		
	// CHECK_CLEAR_STATUS request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_CHECK_CLEAR_STATUS, // INITIATE_CLEAR
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE, // Request type
		0, // Interface number (always zero for USBTMC)
		0, // Reserved
		usbtmc_buffer, // Target buffer
		2, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
			
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: CHECK_CLEAR_STATUS returned %x\n",
		usbtmc_buffer[0]);
	#endif
			
	if(usbtmc_buffer[0]==USBTMC_STATUS_SUCCESS) {
		// Done. No data to read off the device.
		goto usbtmc_clear_bulk_out_halt;
	}
			
	if(usbtmc_buffer[0]!=USBTMC_STATUS_PENDING) {
		printk(KERN_NOTICE "USBTMC: CHECK_CLEAR_STATUS returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}
	
	// Check bmClear field to see if data needs to be read off
	// the device.
			
	if(usbtmc_buffer[1]==1) do {
		// Read a chunk of data from bulk in endpoint
				
		#ifdef USBTMC_DEBUG
		printk(KERN_NOTICE "USBTMC: Reading from bulk in EP\n");
		#endif
						
		// Read from bulk in EP
		rv=usb_bulk_msg(p_device_data->usb_dev,
			usb_rcvbulkpipe(p_device_data->usb_dev,
				p_device_data->bulk_in), // Bulk in pipe
			usbtmc_buffer, // Target buffer
			USBTMC_SIZE_IOBUFFER, // Max characters to read
			&actual, // Actual characters read
			p_device_data->timeout); // Timeout (jiffies)
				
		n++;
				
		if(rv<0) { // I/O error
			printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",
				rv);
			return rv;
		}
	} while ((actual==max_size)&&(n<USBTMC_MAX_READS_TO_CLEAR_BULK_IN));
		
	if(actual==max_size) { // Couldn't clear device buffer
		printk(KERN_NOTICE
			"USBTMC: Couldn't clear device buffer within %d cycles\n",
			USBTMC_MAX_READS_TO_CLEAR_BULK_IN);
		return -EPERM;
	}
			
	// Device should be clear at this point. Now check status again!
	goto usbtmc_clear_check_status;

usbtmc_clear_bulk_out_halt:	
			
	// Finally, clear bulk out halt
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_sndctrlpipe(p_device_data->usb_dev,0), // Control out pipe
		USB_REQ_CLEAR_FEATURE, // Clear feature request
		USB_DIR_OUT|USB_TYPE_STANDARD|USB_RECIP_ENDPOINT, // To EP
		USB_ENDPOINT_HALT, // Feature ENDPOINT_HALT
		p_device_data->bulk_out,
		usbtmc_buffer, // Target buffer
		0, // Although we don't really want data this time
		p_device_data->timeout);
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
			
	return 0;
}

int usbtmc_ioctl_set_attribute (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Set driver attribute.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;

	// Get pointer to private data structure
	p_device_data=filp->private_data;
	
	switch(((struct usbtmc_attribute *)arg)->attribute)
	{
		case USBTMC_ATTRIB_AUTO_ABORT_ON_ERROR:
			if((((struct usbtmc_attribute *)arg)->value!=
				USBTMC_ATTRIB_VAL_ON)&&
				(((struct usbtmc_attribute *)arg)->value!=
					USBTMC_ATTRIB_VAL_OFF))
				return -EINVAL;
			p_device_data->auto_abort=((struct usbtmc_attribute *)arg)->value;
			break;
		
		case USBTMC_ATTRIB_READ_MODE:
			if((((struct usbtmc_attribute *)arg)->value!=
				USBTMC_ATTRIB_VAL_FREAD)&&
				(((struct usbtmc_attribute *)arg)->value!=
					USBTMC_ATTRIB_VAL_READ))
				return -EINVAL;
			p_device_data->fread=((struct usbtmc_attribute *)arg)->value;
			break;
		
		case USBTMC_ATTRIB_TIMEOUT:
			if(((struct usbtmc_attribute *)arg)->value<0)
				return -EINVAL;
			p_device_data->timeout=
				((struct usbtmc_attribute *)arg)->value/1000*HZ;
			break;
				
		case USBTMC_ATTRIB_TERM_CHAR_ENABLED:
			if((((struct usbtmc_attribute *)arg)->value!=
				USBTMC_ATTRIB_VAL_ON)&&
				(((struct usbtmc_attribute *)arg)->value!=
					USBTMC_ATTRIB_VAL_OFF))
				return -EINVAL;
			p_device_data->term_char_enabled=
				((struct usbtmc_attribute *)arg)->value;
			break;

		case USBTMC_ATTRIB_TERM_CHAR:
			if((((struct usbtmc_attribute *)arg)->value<0)||
				(((struct usbtmc_attribute *)arg)->value>255))
				return -EINVAL;
			p_device_data->term_char=
				((struct usbtmc_attribute *)arg)->value;
			break;				
				
		case USBTMC_ATTRIB_ADD_NL_ON_READ:
			if((((struct usbtmc_attribute *)arg)->value!=
				USBTMC_ATTRIB_VAL_ON)&&
				(((struct usbtmc_attribute *)arg)->value!=
					USBTMC_ATTRIB_VAL_OFF))
				return -EINVAL;
			p_device_data->add_nl_on_read=
				((struct usbtmc_attribute *)arg)->value;
			break;
				
		case USBTMC_ATTRIB_REM_NL_ON_WRITE:
			if((((struct usbtmc_attribute *)arg)->value!=
				USBTMC_ATTRIB_VAL_ON)&&
				(((struct usbtmc_attribute *)arg)->value!=
					USBTMC_ATTRIB_VAL_OFF))
				return -EINVAL;
			p_device_data->rem_nl_on_write=
				((struct usbtmc_attribute *)arg)->value;
			break;
				
		default:
			// Bad attribute or read-only
			return -EINVAL;	
	}
		
	return 0;
}

int usbtmc_ioctl_get_attribute (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Read driver attribute.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;
	int n;
	int count;

	// Get pointer to private data structure
	p_device_data=filp->private_data;
	
	switch(((struct usbtmc_attribute *)arg)->attribute)
	{
		case USBTMC_ATTRIB_AUTO_ABORT_ON_ERROR:
			((struct usbtmc_attribute *)arg)->value=p_device_data->auto_abort;
			break;
		
		case USBTMC_ATTRIB_READ_MODE:
			((struct usbtmc_attribute *)arg)->value=p_device_data->fread;
			break;
		
		case USBTMC_ATTRIB_TIMEOUT:
			((struct usbtmc_attribute *)arg)->value=
				p_device_data->timeout/HZ*1000;
			break;
			
		case USBTMC_ATTRIB_NUM_INSTRUMENTS:
			count=0;
			for(n=1;n<USBTMC_MINOR_NUMBERS;n++)
				if(usbtmc_minors[n]) count++;
			((struct usbtmc_attribute *)arg)->value=count;
			break;
			
		case USBTMC_ATTRIB_MINOR_NUMBERS:
			((struct usbtmc_attribute *)arg)->value=
				USBTMC_MINOR_NUMBERS; // defined in usbtmc.h
			break;
			
		case USBTMC_ATTRIB_SIZE_IO_BUFFER:
			((struct usbtmc_attribute *)arg)->value=
				USBTMC_SIZE_IOBUFFER; // defined in usbtmc.h
			break;
			
		case USBTMC_ATTRIB_DEFAULT_TIMEOUT:
			((struct usbtmc_attribute *)arg)->value=
				USBTMC_DEFAULT_TIMEOUT/HZ*1000;
			// defined in usbtmc.h (in jiffies)
			break;

		case USBTMC_ATTRIB_DEBUG_MODE:
			((struct usbtmc_attribute *)arg)->value=0;
			#ifdef USBTMC_DEBUG
			((struct usbtmc_attribute *)arg)->value=1;
			#endif
			// defined at the beginning of this file
			break;
			
		case USBTMC_ATTRIB_VERSION:
			((struct usbtmc_attribute *)arg)->value=USBTMC_VERSION;
			// defined at the beginning of this file
			break;

		case USBTMC_ATTRIB_TERM_CHAR_ENABLED:
			((struct usbtmc_attribute *)arg)->value=
				p_device_data->term_char_enabled;
			break;			
			
		case USBTMC_ATTRIB_TERM_CHAR:
			((struct usbtmc_attribute *)arg)->value=
				p_device_data->term_char;
			break;
			
		case USBTMC_ATTRIB_ADD_NL_ON_READ:
			((struct usbtmc_attribute *)arg)->value=
				p_device_data->add_nl_on_read;
			break;			
			
		case USBTMC_ATTRIB_REM_NL_ON_WRITE:
			((struct usbtmc_attribute *)arg)->value=
				p_device_data->rem_nl_on_write;
			break;
			
		default:
			return -EINVAL;	
	}
		
	return 0;
}

int usbtmc_ioctl_clear_out_halt (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Send CLEAR_FEATURE request to clear bulk out endpoint halt.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;
	int rv;

	// Get pointer to private data structure
	p_device_data=filp->private_data;
		
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_sndctrlpipe(p_device_data->usb_dev,0), // Control out pipe
		USB_REQ_CLEAR_FEATURE, // Clear feature request
		USB_DIR_OUT|USB_TYPE_STANDARD|USB_RECIP_ENDPOINT, // To EP
		USB_ENDPOINT_HALT, // Feature ENDPOINT_HALT
		p_device_data->bulk_out,
		usbtmc_buffer, // Target buffer
		0, // Although we don't really want data this time
		p_device_data->timeout);
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}

	return 0;
}

int usbtmc_ioctl_clear_in_halt (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Send CLEAR_FEATURE request to clear bulk in endpoint halt.
// Normally, you should not need this function. If a read transaction is
// not processed properly (e.g. if a timeout occurs), ABORT_BULK_IN is usually
// a better choice.
// Called by usbtmc_ioctl... See below!
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	
	// Get pointer to private data structure
	p_device_data=filp->private_data;
		
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_sndctrlpipe(p_device_data->usb_dev,0), // Control out pipe
		USB_REQ_CLEAR_FEATURE, // Clear feature request
		USB_DIR_OUT|USB_TYPE_STANDARD|USB_RECIP_ENDPOINT, // To EP
		USB_ENDPOINT_HALT, // Feature ENDPOINT_HALT
		p_device_data->bulk_in,
		usbtmc_buffer, // Target buffer
		0, // Although we don't really want data this time
		p_device_data->timeout);
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
	
	return 0;
}

int usbtmc_ioctl_get_capabilities (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Returns information about the device's optional capabilities.
// See section 4.2.1.8 of the USBTMC specifcation for details.
// Called by usbtmc_ioctl... See below!	
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	
	// Get pointer to private data structure
	p_device_data=filp->private_data;	
		
	// GET_CAPABILITIES request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_GET_CAPABILITIES, // GET_CAPABILITIES
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE, // Request type
		0, // Interface number (always zero for USBTMC)
		0, // Reserved
		usbtmc_buffer, // Target buffer
		0x18, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
			
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
			
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: GET_CAPABILITIES returned %x\n",
		usbtmc_buffer[0]);
	printk(KERN_NOTICE "USBTMC: Interface capabilities are %x\n",
		usbtmc_buffer[4]);
	printk(KERN_NOTICE "USBTMC: Device capabilities are %x\n",
		usbtmc_buffer[5]);
	printk(KERN_NOTICE "USBTMC: USB488 interface capabilities are %x\n",
		usbtmc_buffer[14]);
	printk(KERN_NOTICE "USBTMC: USB488 device capabilities are %x\n",
		usbtmc_buffer[15]);
	#endif
		
	if(usbtmc_buffer[0]!=USBTMC_STATUS_SUCCESS) {
		printk(KERN_NOTICE "USBTMC: GET_CAPABILITIES returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}
			
	((struct usbtmc_dev_capabilities*)arg)->interface_capabilities=
		usbtmc_buffer[4];
	((struct usbtmc_dev_capabilities*)arg)->device_capabilities=
		usbtmc_buffer[5];
	((struct usbtmc_dev_capabilities*)arg)->usb488_interface_capabilities=
		usbtmc_buffer[14];
	((struct usbtmc_dev_capabilities*)arg)->usb488_device_capabilities=
		usbtmc_buffer[15];

	return 0;
}

int usbtmc_ioctl_indicator_pulse (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Turns on the device's activity indicator for identification.
// This capability is optional. When in doubt, use GET_CAPABILITIES to check
// for support.
// Called by usbtmc_ioctl... See below!	
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	
	// Get pointer to private data structure
	p_device_data=filp->private_data;
		
	// INDICATOR_PULSE request
	rv=usb_control_msg(
		p_device_data->usb_dev, // USB device structure
		usb_rcvctrlpipe(p_device_data->usb_dev,0), // Control in pipe
		USBTMC_REQUEST_INDICATOR_PULSE, // INDICATOR_PULSE
		USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE, // Request type
		0, // Interface number (always zero for USBTMC)
		0, // Reserved
		usbtmc_buffer, // Target buffer
		0x01, // Number of characters to read
		p_device_data->timeout); // Timeout (jiffies)
		
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_control_msg returned %d\n",rv);
		return rv;
	}
	
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: INDICATOR_PULSE returned %x\n",
		usbtmc_buffer[0]);
	#endif

	if(usbtmc_buffer[0]!=USBTMC_STATUS_SUCCESS)
	{
		printk(KERN_NOTICE "USBTMC: INDICATOR_PULSE returned %x\n",
			usbtmc_buffer[0]);
		return -EPERM;
	}
			
	return 0;
}

int usbtmc_ioctl_instrument_data (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Fills usbtmc_instrument data structure.
// Called by usbtmc_ioctl... See below!	
{
	struct usbtmc_device_data *p_device_data;
	struct usb_device *p_device;
	int n;
	
	// Get pointer to private data structure
	p_device_data=filp->private_data;
	
	// See if minor number is OK
	if(!usbtmc_minors[((struct usbtmc_instrument *)arg)->minor_number])
		// This minor number is not in use!
		return -EINVAL;
	
	// Now fill the data structure
	p_device=interface_to_usbdev(
		usbtmc_devs[((struct usbtmc_instrument *)arg)->minor_number]->intf);
	// Manufacturer
	n=strlen(p_device->manufacturer);
	if(n>199) n=199;
	if(copy_to_user(((struct usbtmc_instrument *)arg)->manufacturer,
		p_device->manufacturer,n)) {
		// There must have been an addressing problem
		return -EFAULT;
	}
	((struct usbtmc_instrument *)arg)->manufacturer[n]=0;
	// Product
	n=strlen(p_device->product);
	if(n>199) n=199;
	if(copy_to_user(((struct usbtmc_instrument *)arg)->product,
		p_device->product,n)) {
		// There must have been an addressing problem
		return -EFAULT;
	}
	((struct usbtmc_instrument *)arg)->product[n]=0;
	// Serial number
	n=strlen(p_device->product);
	if(n>199) n=199;
	if(copy_to_user(((struct usbtmc_instrument *)arg)->serial_number,
		p_device->serial,n)) {
		// There must have been an addressing problem
		return -EFAULT;
	}
	((struct usbtmc_instrument *)arg)->serial_number[n]=0;
	return 0;
}

int usbtmc_ioctl_reset_conf (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// Reinitialize current USB configuration and its interfaces.
// Called by usbtmc_ioctl... See below!	
{
	struct usbtmc_device_data *p_device_data;
	int rv;
	
	// Get pointer to private data structure
	p_device_data=filp->private_data;

	// Reset configuration
	rv=usb_reset_configuration(p_device_data->usb_dev);
	
	if(rv<0) { // I/O error
		printk(KERN_NOTICE "USBTMC: usb_reset_configuration returned %d\n",rv);
		return rv;
	}
	
	return 0;
}

int usbtmc_ioctl (struct inode *inode,
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
// ioctl is used for special operations (other than message-based I/O), such
// as device clear.
{
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Ioctl function called\n");
	#endif
		
	switch(cmd)
	{
		case USBTMC_IOCTL_SET_ATTRIBUTE:
			return usbtmc_ioctl_set_attribute(inode,filp,cmd,arg);
		
		case USBTMC_IOCTL_GET_ATTRIBUTE:
			return usbtmc_ioctl_get_attribute(inode,filp,cmd,arg);
		
		case USBTMC_IOCTL_CLEAR_OUT_HALT:
			return usbtmc_ioctl_clear_out_halt(inode,filp,cmd,arg);
		
		case USBTMC_IOCTL_CLEAR_IN_HALT:
			return usbtmc_ioctl_clear_in_halt(inode,filp,cmd,arg);
		
		case USBTMC_IOCTL_GET_CAPABILITIES:
			return usbtmc_ioctl_get_capabilities(inode,filp,cmd,arg);
			
		case USBTMC_IOCTL_INDICATOR_PULSE:
			return usbtmc_ioctl_indicator_pulse(inode,filp,cmd,arg);
			
		case USBTMC_IOCTL_CLEAR:
			return usbtmc_ioctl_clear(inode,filp,cmd,arg);
			
		case USBTMC_IOCTL_ABORT_BULK_OUT:
			return usbtmc_ioctl_abort_bulk_out(inode,filp,cmd,arg);
			
		case USBTMC_IOCTL_ABORT_BULK_IN:
			return usbtmc_ioctl_abort_bulk_in(inode,filp,cmd,arg);
		
		case USBTMC_IOCTL_INSTRUMENT_DATA:
			return usbtmc_ioctl_instrument_data(inode,filp,cmd,arg);
		
		case USBTMC_IOCTL_RESET_CONF:
			return usbtmc_ioctl_reset_conf(inode,filp,cmd,arg);
		
		default:
			return -EBADRQC; // Invalid request code
	}
}

loff_t usbtmc_llseek(struct file *filp,loff_t position,int x)
// Seek (random access) doesn't make sense with test instruments, so this
// function returns an error.
{
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_llseek called\n");
	#endif

	return -EPERM; // Operation not permitted
}

// This structure is used to publish the char device driver entry points
static struct file_operations fops = {
	.owner=THIS_MODULE,
	.read=usbtmc_read,
	.write=usbtmc_write,
	.open=usbtmc_open,
	.release=usbtmc_release,
	.ioctl=usbtmc_ioctl,
	.llseek=usbtmc_llseek,
};

static int usbtmc_probe(struct usb_interface *intf,
	const struct usb_device_id *id)
// The probe function is called whenever a device is connected which is serviced
// by this driver (USBTMC device).
{
	int retcode;
	struct usbtmc_device_data *p_device_data;
	struct usb_device *p_device;
	int n;

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_probe called\n");
	#endif
	
	// Wait for completion of driver initialization
	// wait_for_completion(&usbtmc_init_done);
	
	// Allocate memory for device specific data
	if(!(p_device_data=kmalloc(sizeof(struct usbtmc_device_data),GFP_KERNEL))) {
		printk(KERN_ALERT "USBTMC: Unable to allocate kernel memory\n");
		goto exit_kmalloc;
	}
	
	// Find the first free minor number
	n=1;
	while(n<USBTMC_MINOR_NUMBERS && usbtmc_minors[n]!=0) n++;
	if(n==USBTMC_MINOR_NUMBERS) {
		printk(KERN_ALERT "USBTMC: No free minor number found\n");
		retcode=-ENOMEM;
		goto exit_cdev_add;
	}
	
	// Now in use
	usbtmc_minors[n]=1;
	usbtmc_devs[n]=p_device_data;
	
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Using minor number %d\n",n);
	#endif
	
	// Initialize cdev structure for this character device
	// Set cdev structure to zero before calling cdev_init
	memset(&p_device_data->cdev,0,sizeof(struct cdev));
	cdev_init(&p_device_data->cdev,&fops);
	p_device_data->cdev.owner=THIS_MODULE;
	p_device_data->cdev.ops=&fops;
	
	// Identify this instrument
	p_device=interface_to_usbdev(intf);
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: New device attached:\n");
	printk(KERN_NOTICE "USBTMC: Product: %s\n",p_device->product);
	printk(KERN_NOTICE "USBTMC: Manufacturer: %s\n",p_device->manufacturer);
	printk(KERN_NOTICE "USBTMC: Serial number: %s\n",p_device->serial);
	#endif
	
	// Combine major and minor numbers
	printk(KERN_NOTICE "USBTMC: MKDEV\n");
	p_device_data->devno=MKDEV(MAJOR(dev),n);
		
	// Add character device to kernel list
	printk(KERN_NOTICE "USBTMC: CDEV_ADD\n");
	if((retcode=cdev_add(&p_device_data->cdev,p_device_data->devno,1))) {
		printk(KERN_ALERT "USBTMC: Unable to add character device\n");
		goto exit_cdev_add;
	}

	// Store info about USB interface in private data structure
	p_device_data->intf=intf;
	p_device_data->id=id;

	// Store pointer to usb device
	p_device_data->usb_dev=usb_get_dev(interface_to_usbdev(intf));

	// Associate pointer to private data with this interface
	usb_set_intfdata(intf,p_device_data);

	// Initialize USBTMC bTag and other fields
	p_device_data->bTag=1;
	p_device_data->eof=0;
	p_device_data->fread=1;
	p_device_data->timeout=USBTMC_DEFAULT_TIMEOUT;
	p_device_data->term_char_enabled=0;
	p_device_data->term_char='\n';
	p_device_data->add_nl_on_read=0;
	p_device_data->rem_nl_on_write=0;
	return 0;

exit_cdev_add:
	// Free memory for device specific data
	kfree(p_device_data);
	return retcode;
	
exit_kmalloc:
	return -ENOMEM;
}

static void usbtmc_disconnect(struct usb_interface *intf)
// The disconnect function is called whenever a device serviced by the driver is
// disconnected.
{
	struct usbtmc_device_data *p_device_data;

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_disconnect called\n");
	#endif

	// Get pointer to private data
	p_device_data=usb_get_intfdata(intf);
	
	// Update array for minor number usage
	usbtmc_minors[MINOR(p_device_data->devno)]=0;

	// Remove character device from kernel list
	cdev_del(&p_device_data->cdev);
	
	// Decrease use count
	usb_get_dev(p_device_data->usb_dev);

	// Free memory allocated for private data
	kfree(p_device_data);

	return;
}

// This structure is used to pass information about this USB driver to the
// USB core (via usb_register)
static struct usb_driver usbtmc_driver = {
	//.owner=THIS_MODULE,
	.name="USBTMC", // Driver name
	.id_table=usbtmc_devices, // Devices serviced by the driver
	.probe=usbtmc_probe, // Probe function (called when device is connected)
	.disconnect=usbtmc_disconnect // Disconnect function
};

static int usbtmc_init(void)
// This function is called when the driver is inserted into the kernel. It
// initializes and registers the driver.
{
	int retcode;
	int n;
	int devno;

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_init called\n");
	#endif
	
	// Reset usbtmc_minors array
	for(n=0;n<USBTMC_MINOR_NUMBERS;n++) usbtmc_minors[n]=0;
	
	// Dynamically allocate char driver major/minor numbers
	if((retcode=alloc_chrdev_region(&dev, // First major/minor number to use
		0, // First minor number
		USBTMC_MINOR_NUMBERS, // Number of minor numbers to reserve
		"USBTMCCHR" // Char device driver name
		))) {
		printk(KERN_ALERT "USBTMC: Unable to allocate major/minor numbers\n");
		goto exit_alloc_chrdev_region;
	}

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Major number is %d\n", MAJOR(dev));
	#endif
	
	// Allocate I/O buffer
	if(!(usbtmc_buffer=kmalloc(USBTMC_SIZE_IOBUFFER,GFP_KERNEL))) {
		printk(KERN_ALERT "USBTMC: Unable to allocate kernel memory\n");
		retcode=-ENOMEM;
		goto exit_kmalloc;
	}
	
	// Initialize cdev structure for driver communication character device
	// Set cdev structure to zero before calling cdev_init
	memset(&cdev,0,sizeof(struct cdev));
	cdev_init(&cdev,&fops);
	cdev.owner=THIS_MODULE;
	cdev.ops=&fops;
	devno=MKDEV(MAJOR(dev),0);
		
	// Add character device to kernel list
	if((retcode=cdev_add(&cdev,devno,1))) {
		printk(KERN_ALERT "USBTMC: Unable to add character device\n");
		retcode=-ENODEV;
		goto exit_cdev_add;
	}
	
	// Initialize completion for driver initialization
	// init_completion(&usbtmc_init_done);
	
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Registering USB driver\n");
	#endif

	// Register USB driver with USB core
	retcode=usb_register(&usbtmc_driver);
	// complete(&usbtmc_init_done);
	if(retcode) {
		printk(KERN_ALERT "USBTMC: Unable to register driver\n");
		goto exit_usb_register;
	}
	
	return 0; // So far so good

exit_usb_register:
	// Remove character device from kernel list
	cdev_del(&cdev);
	
exit_cdev_add:
	// Free driver buffers
	kfree(usbtmc_buffer);
	
exit_kmalloc:
	// Unregister char driver major/minor numbers
	unregister_chrdev_region(dev,USBTMC_MINOR_NUMBERS);
	
exit_alloc_chrdev_region:
	return retcode;
}

static void usbtmc_exit(void)
// The exit function is called before the driver is unloaded from the kernel.
// It is supposed to clean up and free any resources allocated by the driver.
{
	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: usbtmc_exit called\n");
	printk(KERN_NOTICE "USBTMC: Unregistering major/minor numbers\n");
	#endif
	
	// Unregister char driver major/minor numbers
	unregister_chrdev_region(dev,USBTMC_MINOR_NUMBERS);
	
	// Release IO buffer allocated in usbtmc_init
	kfree(usbtmc_buffer);

	#ifdef USBTMC_DEBUG
	printk(KERN_NOTICE "USBTMC: Deregistering driver\n");
	#endif
	
	// Unregister USB driver with USB core
	usb_deregister(&usbtmc_driver);
}

module_init(usbtmc_init); // Inititialization function
module_exit(usbtmc_exit); // Shutdown function

MODULE_LICENSE("GPL");

// Revision history
//
// 1.0		05.11.2007	Initial version.
// 1.0.1	07.11.2007	Set cdev struct to zero prior to calling cdev_init().
// 1.0.2	09.11.2007	Bug fixes related to control requests.
// 1.0.3	13.11.2007	Automatic ABORT on error capability.
// 1.0.4	21.11.2007	Updated ioctl functions.
//						Added GET_ATTRIBUTE and SET_ATTRIBUTE functions.
//						Added usbtmc_ioctl command line utility.
// 1.1		08.12.2007	Added several new attributes.
//						Wrote HTML documentation.
