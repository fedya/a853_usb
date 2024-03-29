/*
Copyright (C) 2011 Skrilax_CZ
Based on Motorola Usb daemon

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>

#include <cutils/properties.h>
#define LOG_TAG "usbd"
#include <cutils/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>
#include <linux/kernel.h>
#include <sys/param.h>



//#define ARRAY_SIZE(Array) (sizeof(Array)/sizeof(Array[0]))

#define ARRAY_SIZE(a) (sizeof a / sizeof a[0])

#define USBD_VER "1.0_CM"

/* The following defines should be general for all Motorola phones */
 
#define PROPERTY_ADB_ENABLED                "persist.service.adb.enable"

#define PROPERTY_ADB_PHONE		    "ro.usb_mode"

/* usb status */
#define USB_MODEL_NAME_PATH                 "/sys/devices/platform/cpcap_battery/power_supply/usb/model_name"
#define USB_ONLINE_PATH                     "/sys/devices/platform/cpcap_battery/power_supply/usb/online"

/* input from model_name*/
#define USB_INPUT_CABLE_NONE                "none"
#define USB_INPUT_CABLE_NORMAL              "usb"
#define USB_INPUT_CABLE_FACTORY             "factory"

/* cable types */
#define USB_TYPE_CABLE_NONE                 -1
#define USB_TYPE_CABLE_NORMAL                0
#define USB_TYPE_CABLE_FACTORY               1

/* cable events */
#define USBD_EVENT_CABLE_CONNECTED          "cable_connected"
#define USBD_EVENT_CABLE_CONNECTED_FACTORY  "cable_connected_factory"
#define USBD_EVENT_CABLE_DISCONNECTED       "cable_disconnected"

/* adb status */
#define USBD_ADB_STATUS_ON                  "usbd_adb_status_on"
#define USBD_ADB_STATUS_OFF                 "usbd_adb_status_off"

/* event prefixes */
#define USBD_START_PREFIX		    "usbd_start_"
#define USBD_REQ_SWITCH_PREFIX              "usbd_req_switch_"
#define USB_MODE_PREFIX                     "usb_mode_"

/* response suffix */
#define USBD_RESP_OK                        ":ok"
#define USBD_RESP_FAIL                      ":fail"

/* adb suffix */
#define USB_MODE_ADB_SUFFIX                 "_adb"

/* structure */
struct usb_mode_info
{
  const char* apk_mode;
  const char* apk_mode_adb;
  const char* apk_start;
  const char* apk_req_switch;
  
  const char* kern_mode;
  const char* kern_mode_adb;
};

#define USB_MODE_INFO(apk,kern) \
{ \
    .apk_mode =         USB_MODE_PREFIX       apk, \
    .apk_mode_adb =     USB_MODE_PREFIX       apk   USB_MODE_ADB_SUFFIX, \
    .apk_start =        USBD_START_PREFIX      apk,	\
    .apk_req_switch =   USBD_REQ_SWITCH_PREFIX apk, \
    .kern_mode =                              kern, \
    .kern_mode_adb =                          kern  USB_MODE_ADB_SUFFIX, \
}

/* The following defines have matching equivalents in usb.apk
 * and in kernel g_mot_android module (see mot_android.c)
 * if you change them here, don't forget to update them there
 * or you will break usb.
 *
 * On Motorola Milestone, the configuration is altered
 * in a module mot_usb.ko.
 */

/* usb modes for usb.apk */
#define USB_APK_MODE_NGP              "usb_mode_ngp_adb"
#define USB_APK_MODE_MTP              "usb_mode_mtp_adb"
#define USB_APK_MODE_MODEM	      "usb_mode_ngp_adb"
#define USB_APK_MODE_MSC              "usb_mode_msc_adb"

/* usb modes for kernel */
#define USB_KERN_MODE_NET             "eth"
#define USB_KERN_MODE_NGP             "acm_eth_mtp_adb"
#define USB_KERN_MODE_MTP             "mtp_adb"
#define USB_KERN_MODE_MODEM           "acm_eth_adb"
#define USB_KERN_MODE_MSC             "msc_adb"

/* available modes */
static struct usb_mode_info usb_modes[] = 
{
	USB_MODE_INFO(USB_APK_MODE_NGP,	  USB_KERN_MODE_NGP),
	USB_MODE_INFO(USB_APK_MODE_MTP,	  USB_KERN_MODE_MTP),
	USB_MODE_INFO(USB_APK_MODE_MODEM,	  USB_KERN_MODE_MODEM),
	USB_MODE_INFO(USB_APK_MODE_MSC,	  USB_KERN_MODE_MSC),
};

/* File descriptors */
int uevent_fd = -1;
int socket_ev = -1;
int usb_mode_fd = -1;

/* ns needed for daemon */
int ns = -1;

/* Listener sockets' descriptors */
int listeners_fd[0x10];

/* Status variables */
int current_usb_mode = 0;
int usb_cable_type = USB_TYPE_CABLE_NONE;
int usb_online = 0;

/* Opens uevent socked for usbd */
int open_uevent_socket(void)
{
  struct sockaddr_nl addr;
  int sz = 64*1024;

  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  addr.nl_pid = getpid();
  addr.nl_groups = 0xFFFFFFFF;
  
  uevent_fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  
  if (uevent_fd < 0)
  {
    LOGE("open_uevent_socket(): Unable to create uevent socket '%s'\n", strerror(errno));
    return -1;
  }
  
  if (setsockopt(uevent_fd, SOL_SOCKET, SO_RCVBUFFORCE, &sz, sizeof(sz)) < 0) 
  {
    LOGE("open_uevent_socket(): Unable to set uevent socket options '%s'\n", strerror(errno));
    return -1;
  }
  
  if (bind(uevent_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
  {
    LOGE("open_uevent_socket(): Unable to bind uevent socket '%s'\n", strerror(errno));
    return -1;
  }
  
  return 0;
}

int ev_init(void)
{
	int fd;

	char fname[32];
	sprintf(fname, "/dev/socket/usbd");
	fd = open(fname, O_RDWR);
	if (fd < 0)
		return -1;
	fd = open_uevent_socket();
	if (fd >= 0) {
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		fcntl(fd, F_SETFL, O_NONBLOCK);
		LOGI("ev_init function");
		}
    return fd;
    }

/* Phone was started up in normal mode 
 * main(): Phone was started up in normal mod
 * Succesfully returned status
 */

int get_phone_mode(void)
{
        char mode[PROPERTY_VALUE_MAX];

        if (property_get(PROPERTY_ADB_PHONE, mode, NULL)) {
                if (!strcmp(mode, "normal"))
                LOGI("main(): Phone was started up in %s mode", mode);
                }{
       		if (!strcmp(mode, "debug"))
		LOGI("main(): Phone was started up in %s mode", mode);}
        return 0;
}

/* Gets adb status 
 * 4611  4611 I usbd    : ADB status is 1
 * USBD succesfully returned
 * real status of persist.service.adb
 */

int get_adb_enabled_status(void){
	char buff[PROPERTY_VALUE_MAX];
	int ret;
  
	ret = property_get(PROPERTY_ADB_ENABLED, buff, "0");
	if (!ret)
		return -1;
//	LOGI("ADB status is %s", buff);    
	return (!strcmp(buff, "1"));
	}



/* work with /dev/socket/usbd 
 * need send info like @usbd_adb_status_on
 */
int send_data(char *buf, int len){
	return send(ns, buf, len, 0);
}



/* Sends adb status to usb.apk (or other listeners)
 * Need to implement send_data function
 * it must send data to /dev/socket/usbd 
 * data must be like usbd_adb_status_on or cable_connected
 * Look at UsbListener.java
 */

static int usbd_send_adb_status(int status){
  int ret;
  
  if (status == 1)
  {
    LOGD("usbd_send_adb_status(): Send ADB Enable message\n");
    ret = send_data(USBD_ADB_STATUS_ON, strlen(USBD_ADB_STATUS_ON) + 1);
    
  }
  else
  {
    LOGD("usbd_send_adb_status(): Send ADB Disable message\n");
    ret = send_data(USBD_ADB_STATUS_OFF, strlen(USBD_ADB_STATUS_OFF) + 1);
  }
  
  return ret <= 0; //1 = fail
}

/* Get usb mode index */
int usbd_get_mode_index(const char* mode, int apk)
{
  unsigned int i;
  
  for (i = 0; i < ARRAY_SIZE(usb_modes); i++)
  {
    if (apk)
    {
      if (!strcmp(mode, usb_modes[i].apk_mode))
        return i;
    }
    else
    {
      if (!strcmp(mode, usb_modes[i].kern_mode))
        return i;
    }
  }
  
  return -1;
}

/* Sets usb mode */
int usbd_set_usb_mode(int new_mode)
{
	int adb_sts;
	const char* mode_str;

	if (new_mode > 0 && new_mode < ARRAY_SIZE(usb_modes))
	{
	/* Check ADB status */
	adb_sts = get_adb_enabled_status();
    
	/* Moto gadget driver expects us to append "_adb" */
		if (adb_sts == 1)
			mode_str = usb_modes[new_mode].kern_mode_adb;
		else
			mode_str = usb_modes[new_mode].kern_mode;
      
		write(usb_mode_fd, mode_str, strlen(mode_str) + 1);
		return 0;
			}
	else
	{
	LOGE("usbd_set_usb_mode(): Cannot set usb mode to '%d'\n", new_mode);
	return 1;
	}
}

/* Get cable status */
int usbd_get_cable_status(void)
{
  char buf[256];
  FILE* f;
  
  /* get cable type */
  f = fopen(USB_MODEL_NAME_PATH, "r");
  
  if (!f)
  {
    LOGE("usbd_get_cable_status: Unable to open power_supply model_name file '%s'\n", strerror(errno));
    return -errno;
  }
  
  if (!fgets(buf, ARRAY_SIZE(buf), f))
  {
    fclose(f);
    LOGE("usbd_get_cable_status: Unable to read power supply model_name for cable type\n");
    return -EIO;
  }
  
  if (!strcmp(buf, USB_INPUT_CABLE_NORMAL))
    usb_cable_type = USB_TYPE_CABLE_NORMAL;
  else if (!strcmp(buf, USB_INPUT_CABLE_FACTORY))
  {
    usb_cable_type = USB_TYPE_CABLE_FACTORY;
    usbd_set_usb_mode(usbd_get_mode_index(USB_KERN_MODE_NET, 0));
  }
  else 
    usb_cable_type = USB_TYPE_CABLE_NONE;

  fclose(f);
  
  LOGI("usbd_get_cable_status(): cable_type = %s", buf);

  /* get online status */
  f = fopen(USB_ONLINE_PATH, "r");
  
  if (!f)
  {
    LOGE("usbd_get_cable_status: Unable to open power_supply online file '%s'\n", strerror(errno));
    return -errno;
  }
  
  if (!fgets(buf, ARRAY_SIZE(buf), f))
  {
    fclose(f);
    LOGE("usbd_get_cable_status: Unable to read power supply online stat\n");
    return -EIO;
  }
  
  if (!strcmp(buf, "1"))
    usb_online = 1;
  else
    usb_online = 0;   
  /*
  if (!strcmp(buf, "1"))
    send_data(USBD_EVENT_CABLE_CONNECTED, strlen(USBD_EVENT_CABLE_CONNECTED) + 1);
    LOGI("usbd_notify_current_status(): : Notifying App with Current Status : %s", USBD_EVENT_CABLE_CONNECTED);
 */
 fclose(f);

  LOGI("usbd_get_cable_status(): current usb_online = %s", buf);

  if (usb_online = 1)
    send_data(USBD_EVENT_CABLE_CONNECTED, strlen(USBD_EVENT_CABLE_CONNECTED) + 1);
    LOGI("usbd_notify_current_status(): : Notifying App with Current Status : %s", USBD_EVENT_CABLE_CONNECTED);
    return 0;
}


/*AF_UNIX usbd socket */

int init_usbd_socket(void)
{
	struct sockaddr_un addr;
	struct sockaddr_un faddr;

	int sz = 64*1024, flags;
	
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	
	strcpy(addr.sun_path, "/dev/socket/usbd");

	socket_ev = socket(AF_UNIX, SOCK_STREAM, 0);
	unlink(addr.sun_path);
	if (socket_ev < 0)
	{
	LOGE("main(): Unable to create usbd socket '%s'\n", strerror(errno));
	return -1;
	}

	if (bind(socket_ev, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
	LOGE("main(): Unable to bind usbd socket '%s'\n", strerror(errno));
	return -1;
	}

	if(listen(socket_ev, 5) < 0) {
	LOGE("main(): can't listen on socket_ev usbd socket");
	return -1;
	}

	LOGI("main(): Initializing usbd socket");
	return 0;


	}

/* Usbd main */
int main(int argc, char **argv)
{
  char buf[513];
  int len;


  LOGD("main(): Start usbd - version " USBD_VER "\n");

  /* init uevent */
  LOGD("main(): Initializing uevent_socket \n");
  if (open_uevent_socket())
    return -1;
  
  /* open device mode */
  LOGD("main(): Initializing usb_device_mode \n");
  usb_mode_fd = open("/dev/usb_device_mode", O_RDWR);

  get_phone_mode();
//  ev_init();


  if (usb_mode_fd < 0)
  {
    LOGE("main(): Unable to open usb_device_mode '%s'\n", strerror(errno));
    return -errno;
  }

  /* init usdb socket */
  if (init_usbd_socket() < 0)
  {
    LOGE("main(): failed to create socket server '%s'\n", strerror(errno));
    return -errno;
  }
  
  /* init cable status */
  if (usbd_get_cable_status() < 0)
  {
    LOGE("main(): failed to get cable status\n");
    return -1;
  }

 while(1) {
	ns = accept(socket_ev, 0, 0);
	if(ns != -1) {
		usbd_send_adb_status(1);
		usbd_get_cable_status();
		while(len = recv(ns, &buf, 512, 0)) {
			buf[len] = '\0';
			LOGI("receiving shit");
			LOGI("%s", buf);
		}
		close(ns);
	}

}

return 0;
}
