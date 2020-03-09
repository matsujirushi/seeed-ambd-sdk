#include <autoconf.h>
#include <FreeRTOS.h>
#include "task.h"
#include <platform/platform_stdlib.h>
#include <wifi/wifi_conf.h>
#include <lwip_netconf.h>
#include "netif.h"
#include "flash_api.h"
#include "device_lock.h"
#include "wlan_fast_connect/example_wlan_fast_connect.h"
#include "wifi_roaming_plus/example_wifi_roaming_plus.h"
#include "netif/etharp.h"
#include <lwip/sockets.h>
#include <lwip/icmp.h>
#include <lwip/inet_chksum.h>
#include <lwip/ip.h>

#define RSSI_ROAMING_THRESHOLD -65	//when current ap rssi < RSSI_ROAMING_THRESHOLD, start connect to an other better ap.
#define RSSI_SCAN_THRESHOLD -60	//when current ap rssi < RSSI_SCAN_THRESHOLD, start to scan a better ap.
#define MAX_CH_NUM 13		// 2.4G has 13 channel
#define SCAN_BUFLEN 500 	//each scan list length= 14 + ssid_length(32MAX). so SCAN_BUFLEN should be AP_NUM*(14+32) at least

#define  FIND_BETTER_RSSI_DELTA 10
#define MAX_AP_NUM 16
#define AP_VALID_TIME 120

#define CAINIAO_ROAMING_DBG 0 		//for debug log
#if	CAINIAO_ROAMING_DBG
#define ROAMING_DBG	printf
#else
#define ROAMING_DBG
#endif

typedef struct wifi_roaming_ap
{
	u8 	ssid[33];
	u8 	bssid[ETH_ALEN];
	u8	channel;
	rtw_security_t		security_type;
	u8 	password[65];
	u8	key_idx;
	s32	rssi;		
#if CONFIG_LWIP_LAYER
	u8	ip[4];
#endif
}wifi_roaming_ap_t;

//ext info in flash
struct ap_additional_info{
	u8 ap_bssid[ETH_ALEN];
	uint32_t sta_ip;
}ap_additional_info_t;

//info in flash
struct wifi_roaming_data {
	struct wlan_fast_reconnect ap_info;
	u8 num;
	u8 ap_n;
	u32 channel[MAX_CH_NUM];	
	struct ap_additional_info add_ap_info[MAX_AP_NUM];	
}wifi_roaming_data_t;

#define SUPPORT_SCAN_5G_CHANNEL 0
typedef struct channel_plan
{
#if SUPPORT_SCAN_5G_CHANNEL
	u8 channel[18];
#else
	u8 channel[14];
#endif
	u8	len;
}channel_plan_t;
#if SUPPORT_SCAN_5G_CHANNEL
channel_plan_t roaming_channel_plan = {{1,2,3,4,5,6,7,8,9,10,11,12,13,149,153,157,161,165}, 18};
#else
channel_plan_t roaming_channel_plan = {{1,2,3,4,5,6,7,8,9,10,11,12,13}, 13};
#endif

enum {
	FAST_CONNECT_SPECIFIC_CH = 0,
	FAST_CONNECT_ALL_CH  = 1
};

#if CONFIG_LWIP_LAYER
extern struct netif xnetif[NET_IF_NUM]; 
#endif
static wifi_roaming_ap_t *ap_list;
static u8 pscan_enable = _TRUE; // if set _TRUE, please set pscan_channel_list
static u8 pscan_channel_list[]={1};// set by customer
extern int error_flag;
static unsigned short ping_seq = 0;
#if defined(CONFIG_FAST_DHCP) && CONFIG_FAST_DHCP
extern uint32_t offer_ip;
#endif
static u32 scan_times = 0;

static int  wifi_write_ap_info_to_flash_ext(u8 * data, u32 len);
static int  wifi_write_ap_info_to_flash(u8 * data, u32 len);

static void generate_ping_echo(unsigned char *buf, int size)
{
	int i;
	struct icmp_echo_hdr *pecho;

	for(i = 0; i < size; i ++) {
		buf[sizeof(struct icmp_echo_hdr) + i] = (unsigned char) i;
	}

	pecho = (struct icmp_echo_hdr *) buf;
	ICMPH_TYPE_SET(pecho, ICMP_ECHO);
	ICMPH_CODE_SET(pecho, 0);
	pecho->chksum = 0;
	pecho->id = 0xABCD;
	pecho->seqno = htons(++ ping_seq);

	//Checksum includes icmp header and data. Need to calculate after fill up icmp header
	pecho->chksum = inet_chksum(pecho, sizeof(struct icmp_echo_hdr) + size);
}

static int roaming_ping_test(u32_t ip_addr)
{
	int i, ping_socket;
	int pint_timeout = 200;
	struct sockaddr_in to_addr, from_addr;
	struct icmp_echo_hdr *pecho;
	int from_addr_len = sizeof(struct sockaddr);
	int ping_size, reply_size;
	unsigned char *ping_buf, *reply_buf;
	unsigned int ping_time, reply_time;
	struct ip_hdr *iphdr;
	int ping_count = 1;	
	int data_size = 32;
	//int ping_interval = 1;
	u32 ping_flag = 0;
	
	if(data_size > BUF_SIZE){
	    printf("\n\r[ERROR] %s: data size error, can't exceed %d",__func__,BUF_SIZE);
	    return 1;
	}
	ping_size = sizeof(struct icmp_echo_hdr) + data_size;
	ping_buf = pvPortMalloc(ping_size);
   	if(NULL == ping_buf){
        	printf("\n\r[ERROR] %s: Allocate ping_buf failed",__func__);
        	return 1;
  	}
	reply_buf = pvPortMalloc(ping_size);
   	 if(NULL == reply_buf){
        	vPortFree(ping_buf);
        	printf("\n\r[ERROR] %s: Allocate reply_buf failed",__func__);
        	return 1;
    	}

	printf("\n\r[%s] PING %d.%d.%d.%d %d(%d) bytes of data\n", __FUNCTION__, (uint8_t)(ip_addr),(uint8_t)(ip_addr>>8),(uint8_t)(ip_addr>>16),(uint8_t)(ip_addr>>24),\
		data_size, sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr) + data_size);			

	for(i = 0; (i < ping_count) ; i ++) {
		ping_socket = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
		/* lwip 1.4.1 */
		setsockopt(ping_socket, SOL_SOCKET, SO_RCVTIMEO, &pint_timeout, sizeof(pint_timeout));
		
		to_addr.sin_len = sizeof(to_addr);
		to_addr.sin_family = AF_INET;
		to_addr.sin_addr.s_addr =ip_addr;

		generate_ping_echo(ping_buf, data_size);
		sendto(ping_socket, ping_buf, ping_size, 0, (struct sockaddr *) &to_addr, sizeof(to_addr));
		ping_time = xTaskGetTickCount();
		
		if((reply_size = recvfrom(ping_socket, reply_buf, ping_size, 0, (struct sockaddr *) &from_addr, (socklen_t *) &from_addr_len))
			>= (int)(sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) && (from_addr.sin_addr.s_addr == to_addr.sin_addr.s_addr)) {
			reply_time = xTaskGetTickCount();
			
			iphdr = (struct ip_hdr *)reply_buf;
			pecho = (struct icmp_echo_hdr *)(reply_buf + (IPH_HL(iphdr) * 4));

			if((pecho->id == 0xABCD) && (pecho->seqno == htons(ping_seq))) {
				printf("\n\r[%s] %d bytes from %s: icmp_seq=%d time=%d ms", __FUNCTION__, reply_size - sizeof(struct icmp_echo_hdr), inet_ntoa(from_addr.sin_addr), htons(pecho->seqno), (reply_time - ping_time) * portTICK_RATE_MS);
				ping_flag = 1;
			}
			
		}else {
			printf("\n\r[%s] Request timeout for icmp_seq %d\n", __FUNCTION__, ping_seq);
			ping_flag = 0;
		}

		close(ping_socket);
		//vTaskDelay(ping_interval);
		if(ping_flag) {
			ping_seq = 0;
			break;
		}
	}
	vPortFree(ping_buf);
	vPortFree(reply_buf);

	if(!ping_flag){
		ping_seq = 0;
		return 1;
	}

	return 0;
}


static int wlan_fast_connect(struct wifi_roaming_data *data, u8 scan_type)
{
	ROAMING_DBG("%s()", __func__);
	unsigned long tick1 = xTaskGetTickCount();
	unsigned long tick2, tick3, tick4, tick5;

	uint32_t	channel;
	uint32_t    security_type;
	uint8_t     pscan_config;
	char key_id[2] = {0};
	int ret;
	uint32_t wifi_retry_connect = 3; //For fast wifi connect retry
	rtw_network_info_t wifi = {0};
	uint8_t tx_pause_state;
	struct ap_additional_info store_dhcp_info = {0};

#if CONFIG_LWIP_LAYER
	netif_set_up(&xnetif[0]);
#endif
	//disable autoreconnect to manually reconnect the specific ap or channel.
#if CONFIG_AUTO_RECONNECT
	wifi_set_autoreconnect(0);
#endif

	memcpy(psk_essid, data->ap_info.psk_essid, sizeof(data->ap_info.psk_essid));
	memcpy(psk_passphrase, data->ap_info.psk_passphrase, sizeof(data->ap_info.psk_passphrase));
	memcpy(wpa_global_PSK, data->ap_info.wpa_global_PSK, sizeof(data->ap_info.wpa_global_PSK));

	channel = data->ap_info.channel;
	sprintf(key_id,"%d",(char) (channel>>28));
	channel &= 0xff;
	security_type = data->ap_info.security_type;
	pscan_config = PSCAN_ENABLE | PSCAN_FAST_SURVEY;
	//set partial scan for entering to listen beacon quickly
WIFI_RETRY_LOOP:
	if(scan_type == FAST_CONNECT_SPECIFIC_CH) {
		ret = wifi_set_pscan_chan((uint8_t *)&channel, &pscan_config, 1);
		if(ret < 0)
			return -1;
	}
	wifi.security_type = security_type;
	strcpy((char *)wifi.ssid.val, (char*)psk_essid);
	wifi.ssid.len = strlen((char*)psk_essid);

	switch(security_type){
		case RTW_SECURITY_WEP_PSK:
			wifi.password = (unsigned char*) psk_passphrase;
			wifi.password_len = strlen((char*)psk_passphrase);
			wifi.key_id = atoi((const char *)key_id);
			break;
		case RTW_SECURITY_WPA_TKIP_PSK:
		case RTW_SECURITY_WPA2_AES_PSK:
#ifdef CONFIG_SAE_SUPPORT
		case RTW_SECURITY_WPA3_AES_PSK:
#endif
			wifi.password = (unsigned char*) psk_passphrase;
			wifi.password_len = strlen((char*)psk_passphrase);
			break;
		default:
			break;
	}

	// 1.connect
	wifi_set_tx_pause_data(ENABLE);
	ret = wifi_connect((char*)wifi.ssid.val, wifi.security_type, (char*)wifi.password, wifi.ssid.len,
		wifi.password_len, wifi.key_id, NULL);
	wifi_set_tx_pause_data(DISABLE);
	tick2 = xTaskGetTickCount();
		
	if(ret != RTW_SUCCESS) {
		wifi_retry_connect--;
		if(wifi_retry_connect > 0){
                	printf("wifi retry connect\r\n");
                     	goto WIFI_RETRY_LOOP;
		}
	}
	// 2.dhcp
	if(ret == RTW_SUCCESS) {
		//Use ping test to check if need to do dhcp.
		if(roaming_ping_test(xnetif[0].gw.addr)) {
			tick4 = xTaskGetTickCount();
#if defined(CONFIG_FAST_DHCP) && CONFIG_FAST_DHCP
		//get offer ip in flash
		if((data->ap_n < MAX_AP_NUM) && (scan_type == FAST_CONNECT_SPECIFIC_CH)) {
			int i = 0;
			for(i = 0; i < data->ap_n; i++) {
				if(memcmp(ap_list->bssid, data->add_ap_info[i].ap_bssid, ETH_ALEN) == 0){
					offer_ip = data->add_ap_info[i].sta_ip;
					ROAMING_DBG("\n\r Find the ehter_addr in flash() \n");	
					break;
				}
			}
		}		
#endif
			LwIP_DHCP(0, DHCP_START);
			tick5 = xTaskGetTickCount();
			printf("dhcp time %d\n",(tick5 - tick4));
			//clean arp? old arp table may not update.
			etharp_cleanup_netif(&xnetif[0]);
			
			//store dhcp info for each ap.
			wifi_get_ap_bssid(store_dhcp_info.ap_bssid);
			store_dhcp_info.sta_ip = xnetif[0].ip_addr.addr;
			wifi_write_ap_info_to_flash_ext((u8 *)&store_dhcp_info, sizeof(struct ap_additional_info));
		}
		error_flag = RTW_NO_ERROR;
	}
		
#if CONFIG_AUTO_RECONNECT
	wifi_set_autoreconnect(2);
#endif
	tick3 = xTaskGetTickCount();
	printf("\n\r == Roaming connect done  after %d ms = %d ms (connection) + %d ms (DHCP) ==\n",(tick3 - tick1), (tick2- tick1), (tick3 - tick2));
	return ret;
}

static int  wifi_write_ap_info_to_flash_ext(u8 * data, u32 len)
{
	(void )len;
	flash_t flash;
	u8 n = 0;
	struct wifi_roaming_data read_data = {0};
	u32 tick1 =xTaskGetTickCount();

	ROAMING_DBG("%s()\n",__FUNCTION__);
	if(!data)
		return -1;

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_stream_read(&flash, FAST_RECONNECT_DATA, sizeof(struct  wifi_roaming_data), (u8 *) &read_data);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	if(read_data.ap_n == 0xff)
		read_data.ap_n = 0;

	//store the dhcp_info into flash after dhcp done
	ROAMING_DBG("\r\n Add additional AP info into flash\n");	
	if(read_data.ap_n < MAX_AP_NUM) {
		for(n=(read_data.ap_n + 1); n>0; n--) {
			if(memcmp(read_data.add_ap_info[n-1].ap_bssid,((struct ap_additional_info *)data)->ap_bssid, 6)==0) {
				read_data.add_ap_info[n].sta_ip = ((struct ap_additional_info*)data)->sta_ip;
				ROAMING_DBG("Have stored this bssid\n");
				break;					
			}
		}
		if(n == 0) {
				read_data.ap_n ++;
				memcpy((u8 *)(read_data.add_ap_info[read_data.ap_n -1].ap_bssid), ((struct ap_additional_info *)data)->ap_bssid, 6);
				read_data.add_ap_info[read_data.ap_n -1].sta_ip = ((struct ap_additional_info *)data)->sta_ip;

				device_mutex_lock(RT_DEV_LOCK_FLASH);
				flash_erase_sector(&flash, FAST_RECONNECT_DATA);	
				flash_stream_write(&flash, FAST_RECONNECT_DATA, sizeof(struct  wifi_roaming_data), (uint8_t *)  &read_data);
				device_mutex_unlock(RT_DEV_LOCK_FLASH);
		}
	}else {
		ROAMING_DBG("%s(): For more AP infos, Please change MAX_AP_NUM first!\n", __func__);
	}
	u32 tick2 =xTaskGetTickCount();
	printf("write ap_info_ext to flash [%d]ms",(tick2 - tick1));
	return 0;
}
	
static int  wifi_write_ap_info_to_flash(u8 * data, u32 len)
{
	(void )len;
	flash_t flash;
	u8 i=0;
	struct wifi_roaming_data read_data = {0};
	u8 ap_change = 0;
	u32 tick1 =xTaskGetTickCount();

	ROAMING_DBG("%s()\n",__FUNCTION__);
	if(!data)
            return -1;

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_stream_read(&flash, FAST_RECONNECT_DATA, sizeof(struct  wifi_roaming_data), (u8 *) &read_data);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	if(read_data.num == 0xff)
		read_data.num = 0;

	//check is common info changed? Is this a new channel?
	if(read_data.num){
		/*check if ap info {ssid/password/security_type} has changed*/
		if(memcmp((u8 *)((struct wlan_fast_reconnect *)data)->psk_essid, (u8 *) (read_data.ap_info.psk_essid), 32)) {
			printf("ap ssid change\n");
			ap_change = 1;
		} else if(memcmp((u8 *)((struct wlan_fast_reconnect *)data)->psk_passphrase, (u8 *) (read_data.ap_info.psk_passphrase), 32)) {
			printf("ap password change\n");
			ap_change = 1;
		} else if(((struct wlan_fast_reconnect *)data)->security_type != read_data.ap_info.security_type) {
			printf("ap security type change\n");
			ap_change = 1;
		} else { /*ap info doesn't change*/
			for(i = 0;i < read_data.num; i++){
				if(read_data.channel[i] == ((struct wlan_fast_reconnect *)data)->channel) {
					ROAMING_DBG("Already stored this channel(%d)\n",((struct wlan_fast_reconnect *)data)->channel);
					 return -1;
				}		
			}
		}		
	}
	if(ap_change) {
		printf("erase flash and restore new ap info\n");
		memset((u8 *)&read_data,0,sizeof(struct  wifi_roaming_data));
		read_data.num = 1;
	}else {
		printf("Add a new channel into flash\n");
		read_data.num++;
	}
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_erase_sector(&flash, FAST_RECONNECT_DATA);	
	read_data.channel[read_data.num-1] = ((struct wlan_fast_reconnect *)data)->channel;//store channel
	//only first ap's detail info has to be stored.
	if(read_data.num == 1)
		memcpy((u8 *)&read_data.ap_info, data,sizeof(struct wlan_fast_reconnect));//store fast connect info
	flash_stream_write(&flash, FAST_RECONNECT_DATA, sizeof(struct  wifi_roaming_data), (uint8_t *)  &read_data);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	u32 tick2 =xTaskGetTickCount();
	printf("write ap_info into flash [%d]ms\n",(tick2-tick1));
	return 0;
}

static int wifi_init_done_callback_roaming(void)
{
	flash_t flash;
	struct wifi_roaming_data read_data = {0};

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	flash_stream_read(&flash, FAST_RECONNECT_DATA, sizeof(struct  wifi_roaming_data), (u8 *) &read_data);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	/* Check whether stored flash profile is empty */
	if((read_data.num == 0) || (read_data.num > MAX_CH_NUM))
		printf("Fast connect profile is empty, abort fast connection\n");
	/* Find the best ap in flash profile */
	else {
		ROAMING_DBG("[Fast connect] Connect to the best ap\n");
		wlan_fast_connect(&read_data,FAST_CONNECT_ALL_CH);
	}
	return 0;
}

void example_wifi_roaming_plus_init(void)
{	
	// Call back from wlan driver after wlan init done
	p_wlan_init_done_callback = wifi_init_done_callback_roaming;

	// Call back from application layer after wifi_connection success
	p_write_reconnect_ptr = wifi_write_ap_info_to_flash;
}


static u32 wifi_roaming_plus_find_ap_from_scan_buf(char*buf, int buflen, char *target_ssid, void *user_data)
{
	u32 target_security = *(u32 *)user_data;
	
	int plen = 0;	
	while(plen < buflen)
	{
		u32 len, security_mode, security_type, channel;
		u32 ssid_len;
		s32 rssi;
		u8 *mac;
		u8 *ssid;				
		// len
		len = (int)*(buf + plen);
		// check end
		if(len == 0|| len == strlen(target_ssid)) break;
		// if len == ssid_len, it means driver dont do scan,maybe it is busy now, buf detail is the same as it initialized
		// mac
		mac =(u8*)(buf + plen + 1);
		// rssi
		rssi = *(s32*)(buf + plen + 1 + 6);
		// security_mode offset = 11
		security_mode = (u8)*(buf + plen + 1 + 6 + 4);
		switch(security_mode){
			case IW_ENCODE_ALG_NONE:
				security_type = RTW_SECURITY_OPEN;
				break;
			case IW_ENCODE_ALG_WEP:
				security_type = RTW_SECURITY_WEP_PSK;
				break;
			case IW_ENCODE_ALG_TKIP:
				security_type = RTW_SECURITY_WPA_TKIP_PSK;
				break;
			case IW_ENCODE_ALG_CCMP:
				security_type = RTW_SECURITY_WPA2_AES_PSK;
				break;
			default:
				break;
		}
		// channel
		channel = *(buf + plen + 1 + 6 + 4 + 1 + 1);
		// ssid
		ssid_len = len - 1 - 6 - 4 - 1 - 1 - 1;
		ssid = (u8*)(buf + plen + 1 + 6 + 4 + 1 + 1 + 1);
		ROAMING_DBG("Scan ap:"MAC_FMT"(%d)\n", MAC_ARG(mac),channel);
		if(target_security == security_type ||
		((target_security & (WPA2_SECURITY|WPA_SECURITY))&&(target_security & (WPA2_SECURITY|WPA_SECURITY)))){
			if(ap_list->rssi < rssi){
				ROAMING_DBG("rssi(%d) is better than last(%d)\n",rssi,ap_list->rssi);
				memset(ap_list, 0 , sizeof(wifi_roaming_ap_t));
				memcpy(ap_list->bssid, mac, ETH_ALEN);
				ap_list->channel = channel;
				ap_list->rssi = rssi;
			}
		}
		plen += len;			
	}
	return 0;
}

int wifi_roaming_scan(struct wifi_roaming_data  read_data, s32 cur_rssi)
{
	wifi_roaming_ap_t	roaming_ap;
	rtw_wifi_setting_t	setting;
	u8	pscan_config;
	channel_plan_t channel_plan_temp = roaming_channel_plan;
	u8 ch = 0;
	u8  tx_pause_state;
	unsigned long tick1 = xTaskGetTickCount();
	
	memset(&setting, 0, sizeof(rtw_wifi_setting_t));
	memset(&roaming_ap, 0, sizeof(wifi_roaming_ap_t));
	roaming_ap.rssi = -100;

	wifi_get_setting(WLAN0_NAME,&setting);
	strcpy((char*)roaming_ap.ssid, (char const*)setting.ssid);
	roaming_ap.security_type =  setting.security_type;
	strcpy((char*)roaming_ap.password, (char const*)setting.password);
	roaming_ap.key_idx = setting.key_idx;

	if(RTW_ERROR == wifi_get_ap_bssid(roaming_ap.bssid))
		printf("\r\n get AP BSSID FAIL!");
	
	if(pscan_enable == _TRUE){
		/*scan specific channels*/
		if(0 < read_data.num && read_data.num < MAX_CH_NUM){
			ROAMING_DBG("\r\n %s():try to find a better ap in flash\n", __func__);
			while(read_data.num) {
				pscan_channel_list[0]=read_data.channel[read_data.num - 1];
				read_data.num--;
				pscan_config = PSCAN_ENABLE;
				wifi_set_pscan_chan(pscan_channel_list, &pscan_config, 1);
				printf("scan(%d)\n",pscan_channel_list[0]);
				wifi_set_tx_pause_data(ENABLE);
				//printf("tx pause: %x", tx_pause_state);
				wifi_scan_networks_with_ssid((int (*)(char *, int, char *, void *))wifi_roaming_plus_find_ap_from_scan_buf, 
					(void *)&roaming_ap.security_type, SCAN_BUFLEN, (char*)roaming_ap.ssid, strlen((char const*)roaming_ap.ssid));
				//	(void *)&target_security, SCAN_BUFLEN, (char*)read_data.ap_info.psk_essid, strlen((char const*)read_data.ap_info.psk_essid));
				wifi_set_tx_pause_data(DISABLE);
				//printf("tx pause: %x", tx_pause_state);
				printf("scan done(%d)\n",pscan_channel_list[0]);
				channel_plan_temp.channel[pscan_channel_list[0] -1] = 0;
				vTaskDelay(500);
			}
		}
		
		if(ap_list->rssi - cur_rssi > FIND_BETTER_RSSI_DELTA) {
			ROAMING_DBG("\r\n %s():Find the best ap in flash successful, rssi = %d\n", __func__,ap_list->rssi);
		} else {
			/*scan other channels*/
			ROAMING_DBG("\r\n %s():Find the best ap in flash fail, rssi = %d, try to find in other channels\n", __func__,ap_list->rssi);
			//for((ch = LWIP_RAND()%39);ch < channel_plan_temp.len;ch++) {
			for(ch =0 ;ch < channel_plan_temp.len;ch++) {
				if(channel_plan_temp.channel[ch]) {
					pscan_channel_list[0]=channel_plan_temp.channel[ch];
					pscan_config = PSCAN_ENABLE;
					printf("scan(%d)\n",pscan_channel_list[0]);
					wifi_set_pscan_chan(pscan_channel_list, &pscan_config, 1);
					wifi_set_tx_pause_data(ENABLE);
					wifi_scan_networks_with_ssid((int (*)(char *, int, char *, void *))wifi_roaming_plus_find_ap_from_scan_buf, 
						(void *)&roaming_ap, SCAN_BUFLEN, (char*)roaming_ap.ssid, strlen((char const*)roaming_ap.ssid));
					wifi_set_tx_pause_data(DISABLE);
					printf("scan(%d) done!\n",pscan_channel_list[0]);
					vTaskDelay(500);
				}
			}
		}
	}else {
		//wifi_set_tx_pause_data(ENABLE, &tx_pause_state);
		wifi_scan_networks_with_ssid((int (*)(char *, int, char *, void *))wifi_roaming_plus_find_ap_from_scan_buf, 
			(void *)&roaming_ap, SCAN_BUFLEN, (char*)roaming_ap.ssid, strlen((char const*)roaming_ap.ssid));
		//wifi_set_tx_pause_data(DISABLE, &tx_pause_state);
	}
	unsigned long tick2 = xTaskGetTickCount();
	printf("scan() time: %d", (tick2-tick1));
	if((memcmp(roaming_ap.bssid, ap_list->bssid, ETH_ALEN)) && ((ap_list->rssi - cur_rssi) > FIND_BETTER_RSSI_DELTA)) {
		printf("\r\n %s():Find a ap: "MAC_FMT"(%d)\n", __func__, MAC_ARG(ap_list->bssid),ap_list->channel);
		scan_times = 0;
		return 1;
	}else {
		ROAMING_DBG("\r\n %s():Didn't find anyother ap\n", __func__);
		scan_times ++;
		return 0;
	}
}

void wifi_roaming_plus_thread(void *param)
{
	(void )param;
	ROAMING_DBG("\n %s()\n",__func__);
	int	ap_rssi;
	u32	polling_count =0;
	u32 ap_valid = AP_VALID_TIME;
	struct wifi_roaming_data read_data = {0};
	flash_t flash;

	while(1){//wait wifi connect
		if(wifi_is_up(RTW_STA_INTERFACE)&&(RTW_SUCCESS == wifi_is_ready_to_transceive(RTW_STA_INTERFACE)))
			break;
		else
			vTaskDelay(1000);
	}
	vTaskDelay(10000);//wait rssi stable

	while(1) {
		if(wifi_is_up(RTW_STA_INTERFACE)&&(RTW_SUCCESS == wifi_is_ready_to_transceive(RTW_STA_INTERFACE))) {	
			wifi_get_rssi(&ap_rssi);
			printf("\r\n %s():Current rssi:%d)\n", __func__,ap_rssi);
			if(ap_rssi < RSSI_SCAN_THRESHOLD) {
				if( polling_count >= 1) {
					ROAMING_DBG("\r\n %s():Start scan, current rssi(%d) < scan threshold rssi(%d) \n", __func__,ap_rssi,RSSI_SCAN_THRESHOLD);
					ap_list = (wifi_roaming_ap_t *)malloc(sizeof(wifi_roaming_ap_t));
					memset(ap_list, 0, sizeof(wifi_roaming_ap_t));
					ap_list->rssi = -100;
					device_mutex_lock(RT_DEV_LOCK_FLASH);
					flash_stream_read(&flash, FAST_RECONNECT_DATA, sizeof(struct  wifi_roaming_data), (u8 *) &read_data);
					device_mutex_unlock(RT_DEV_LOCK_FLASH);
	
					/*1.find a better ap*/
					if(wifi_roaming_scan(read_data,ap_rssi)) {
						ap_valid = AP_VALID_TIME;
						ROAMING_DBG("\r\n %s():Find a better ap, rssi(%d)\n", __func__,ap_list->rssi);
						
						while(ap_valid){				
							if(wifi_is_up(RTW_STA_INTERFACE)&&(RTW_SUCCESS == wifi_is_ready_to_transceive(RTW_STA_INTERFACE))){
								wifi_get_rssi(&ap_rssi);
								ROAMING_DBG("\r\n %s():Current rssi(%d),check if need to connect an other ap\n", __func__,ap_rssi);
								if( (ap_rssi < RSSI_ROAMING_THRESHOLD)){
									scan_times = 1;
									/*2.connect a better ap*/
									ROAMING_DBG("\r\n %s():Start roaming, current rssi(%d) < roaming threshold rssi(%d),target ap rssi(%d)\n", \
										__func__,ap_rssi,RSSI_ROAMING_THRESHOLD,ap_list->rssi);
									read_data.ap_info.channel = ap_list->channel;
									wlan_fast_connect(&read_data,FAST_CONNECT_SPECIFIC_CH);
									break;
								}
								else if(ap_rssi > RSSI_SCAN_THRESHOLD + 5) {
									/*no need to roaming*/
									ROAMING_DBG("\r\n %s():Current rssi=%d,no need to roaming\n",__func__,ap_rssi);
									break;
								}
								ap_valid--;
							} else 
								break;
							vTaskDelay(1000);
						}
					} else
						ROAMING_DBG("No other better APs!\n ");
					free(ap_list);
					polling_count = 0;	
				}else				
				polling_count++;
			}else
				polling_count = 0;
		}
		vTaskDelay(1000);
	}
	vTaskDelete(NULL);
}

void example_wifi_roaming_plus(void)
{
	if(xTaskCreate(wifi_roaming_plus_thread, ((const char*)"wifi_roaming_thread"), 1024, NULL, tskIDLE_PRIORITY + 1 , NULL) != pdPASS)
		printf("\n\r%s xTaskCreate(wifi_roaming_thread) failed", __FUNCTION__);
	return;
}

