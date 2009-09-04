// usbtmc_ioctl.c
// This file is part of a Linux kernel module for USBTMC (USB Test and
// Measurement Class) devices. It allows access to the driver's ioctl
// entry point.
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

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include "usbtmc.h"

int minor_number=0;
char devfile[100];
int request=-1;
int myfile;
int rv;
struct usbtmc_dev_capabilities devcaps;
struct usbtmc_attribute attr;

int main(int argc,char *argv[])
{
	if(argc<3) goto print_usage;
	
	// Convert parameter #1 (minor number)
	sscanf(argv[1],"%d",&minor_number);
	if((minor_number<1)||(minor_number>USBTMC_MINOR_NUMBERS)) {
		printf("Error: Bad minor number.\n");
		goto print_usage;
	}
	sprintf(devfile,"/dev/usbtmc%d",minor_number);
	
	// Convert parameter #2 (request name)
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_GET_CAPABILITIES))
		request=USBTMC_IOCTL_GET_CAPABILITIES;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_INDICATOR_PULSE))
		request=USBTMC_IOCTL_INDICATOR_PULSE;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_CLEAR))
		request=USBTMC_IOCTL_CLEAR;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_ABORT_BULK_OUT))
		request=USBTMC_IOCTL_ABORT_BULK_OUT;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_ABORT_BULK_IN))
		request=USBTMC_IOCTL_ABORT_BULK_IN;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_SET_ATTRIBUTE))
		request=USBTMC_IOCTL_SET_ATTRIBUTE;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_CLEAR_OUT_HALT))
		request=USBTMC_IOCTL_CLEAR_OUT_HALT;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_CLEAR_IN_HALT))
		request=USBTMC_IOCTL_CLEAR_IN_HALT;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_GET_ATTRIBUTE))
		request=USBTMC_IOCTL_GET_ATTRIBUTE;
	if(!strcmp(argv[2],USBTMC_IOCTL_NAME_RESET_CONF))
		request=USBTMC_IOCTL_RESET_CONF;
	
	// Open device file
	myfile=open(devfile,O_RDWR);
	if(myfile==-1) {
		printf("Error: Can't open device file %s.\n",devfile);
		exit(-1);
	}
	
	switch(request) {
		case USBTMC_IOCTL_INDICATOR_PULSE:
		case USBTMC_IOCTL_CLEAR:
		case USBTMC_IOCTL_ABORT_BULK_OUT:
		case USBTMC_IOCTL_ABORT_BULK_IN:
		case USBTMC_IOCTL_CLEAR_OUT_HALT:
		case USBTMC_IOCTL_CLEAR_IN_HALT:
		case USBTMC_IOCTL_RESET_CONF:
			
			rv=ioctl(myfile,request,0);
			if(rv==-1) {
				printf("Error: ioctl returned %d.\n",rv);
			}
			break;
			
		case USBTMC_IOCTL_GET_CAPABILITIES:
			rv=ioctl(myfile,request,&devcaps);
			printf("Interface capabilities: %u\n",
				devcaps.interface_capabilities);
			printf("Device capabilities: %u\n",
				devcaps.device_capabilities);
			printf("USB488 interface capabilities: %u\n",
				devcaps.usb488_interface_capabilities);
			printf("USB488 device capabilities: %u\n",
				devcaps.usb488_device_capabilities);	
			break;
		
		case USBTMC_IOCTL_SET_ATTRIBUTE:
		case USBTMC_IOCTL_GET_ATTRIBUTE:
			// Convert parameter #3 (attribute name)
			attr.attribute=-1;
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_AUTO_ABORT_ON_ERROR))
				attr.attribute=USBTMC_ATTRIB_AUTO_ABORT_ON_ERROR;
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_READ_MODE))
				attr.attribute=USBTMC_ATTRIB_READ_MODE;
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_TIMEOUT))
				attr.attribute=USBTMC_ATTRIB_TIMEOUT;
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_NUM_INSTRUMENTS))
				attr.attribute=USBTMC_ATTRIB_NUM_INSTRUMENTS;
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_MINOR_NUMBERS))
				attr.attribute=USBTMC_ATTRIB_MINOR_NUMBERS;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_SIZE_IO_BUFFER))
				attr.attribute=USBTMC_ATTRIB_SIZE_IO_BUFFER;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_DEFAULT_TIMEOUT))
				attr.attribute=USBTMC_ATTRIB_DEFAULT_TIMEOUT;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_DEBUG_MODE))
				attr.attribute=USBTMC_ATTRIB_DEBUG_MODE;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_VERSION))
				attr.attribute=USBTMC_ATTRIB_VERSION;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_TERM_CHAR_ENABLED))
				attr.attribute=USBTMC_ATTRIB_TERM_CHAR_ENABLED;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_TERM_CHAR))
				attr.attribute=USBTMC_ATTRIB_TERM_CHAR;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_ADD_NL_ON_READ))
				attr.attribute=USBTMC_ATTRIB_ADD_NL_ON_READ;			
			if(!strcmp(argv[3],USBTMC_ATTRIB_NAME_REM_NL_ON_WRITE))
				attr.attribute=USBTMC_ATTRIB_REM_NL_ON_WRITE;			
			if(attr.attribute==-1) {
				printf("Error: Bad attribute name\n");
				close(myfile);
				goto print_usage;
			}
			if(request==USBTMC_IOCTL_SET_ATTRIBUTE) {
				attr.value=-1;
				if(!strcmp(argv[4],USBTMC_ATTRIB_NAME_VAL_OFF))
					attr.value=USBTMC_ATTRIB_VAL_OFF;
				if(!strcmp(argv[4],USBTMC_ATTRIB_NAME_VAL_ON))
					attr.value=USBTMC_ATTRIB_VAL_ON;
				if(!strcmp(argv[4],USBTMC_ATTRIB_NAME_VAL_FREAD))
					attr.value=USBTMC_ATTRIB_VAL_FREAD;
				if(!strcmp(argv[4],USBTMC_ATTRIB_NAME_VAL_READ))
					attr.value=USBTMC_ATTRIB_VAL_READ;
				if(attr.value==-1) sscanf(argv[4],"%d",&attr.value);
			}
			rv=ioctl(myfile,request,&attr);
			if(request==USBTMC_IOCTL_GET_ATTRIBUTE) {
				if((attr.attribute==USBTMC_ATTRIB_AUTO_ABORT_ON_ERROR)||
					(attr.attribute==USBTMC_ATTRIB_DEBUG_MODE)||
					(attr.attribute==USBTMC_ATTRIB_TERM_CHAR_ENABLED)||
					(attr.attribute==USBTMC_ATTRIB_ADD_NL_ON_READ)||
					(attr.attribute==USBTMC_ATTRIB_REM_NL_ON_WRITE))
					if(attr.value==USBTMC_ATTRIB_VAL_OFF)
						printf("Value: %s\n",USBTMC_ATTRIB_NAME_VAL_OFF);
					else
						printf("Value: %s\n",USBTMC_ATTRIB_NAME_VAL_ON);
				if(attr.attribute==USBTMC_ATTRIB_READ_MODE)
					if(attr.value==USBTMC_ATTRIB_VAL_FREAD)
						printf("Value: %s\n",USBTMC_ATTRIB_NAME_VAL_FREAD);
					else
						printf("Value: %s\n",USBTMC_ATTRIB_NAME_VAL_READ);
				if(attr.attribute==USBTMC_ATTRIB_TIMEOUT)
					printf("Value: %d\n",attr.value);
				if(attr.attribute==USBTMC_ATTRIB_NUM_INSTRUMENTS)
					printf("Value: %d\n",attr.value);
				if(attr.attribute==USBTMC_ATTRIB_MINOR_NUMBERS)
					printf("Value: %d\n",attr.value);
				if(attr.attribute==USBTMC_ATTRIB_SIZE_IO_BUFFER)
					printf("Value: %d\n",attr.value);
				if(attr.attribute==USBTMC_ATTRIB_DEFAULT_TIMEOUT)
					printf("Value: %d\n",attr.value);
				if(attr.attribute==USBTMC_ATTRIB_VERSION)
					printf("Value: %d\n",attr.value);
				if(attr.attribute==USBTMC_ATTRIB_TERM_CHAR)
					printf("Value: %d\n",attr.value);					
			}
			break;
			
			default:
				printf("Error: Bad request name.\n");
				close(myfile);
				goto print_usage;
	}		
		
	// Close device file
	close(myfile);
	
	exit(0);
	
print_usage:
	
	printf("Usage:\n");
	printf("usbtmc_ioctl n request [ attribute [ value ] ]\n");
	printf("where\n");
	printf("m = minor number, e. g. 1 for /dev/usbtmc1\n");
	printf("request = { clear , setattr , getattr , reset etc}\n");
	printf("attribute = { autoabort , readmode , timeout etc }\n");
	printf("See html documentation for details!\n");
	printf("Example:\n");
	printf("usbtmc_ioctl 1 clear\n");
	printf("Clears input and output buffer of device /dev/usbtmc1\n");
	exit(-1);
}


// Revision history
//
// 1.0		05.11.2007	Initial version.
// 1.0.1	07.11.2007	Set cdev struct to zero prior to calling cdev_init().
// 1.0.2	09.11.2007	Bug fixes related to control requests.
// 1.0.3	13.11.2007	Automatic ABORT on error in FREAD (shell) mode.
// 1.0.4	02.12.2007	Added a whole bunch of attributes.
// 1.1		08.12.2007	Clean-up.
