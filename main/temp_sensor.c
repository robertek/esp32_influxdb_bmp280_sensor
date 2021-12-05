/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2021, Robert David <robert.david@posteo.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_http_client.h"

#include "bmp280_ulp_driver.h"


#define INFLUX_URL "http://" CONFIG_INFLUX_IP ":" CONFIG_INFLUX_PORT \
    "/write?db=" CONFIG_INFLUX_DB
#define INFLUX_TAG "baro,site=" CONFIG_INFLUX_SITE ",place=" CONFIG_INFLUX_PLACE

#define BUF_SIZE 128


#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
int s_retry_num = 0;


/*
 * Generic WIFI event handler taken from examples.
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT &&
	    event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < CONFIG_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(__func__, "retry to connect to the AP");
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(__func__, "connect to the AP failed");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(__func__, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

/*
 * Connect to the WIFI AP.
 */
static int wifi_start()
{
	esp_err_t ret = ESP_OK;
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	EventBits_t bits;
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PASSWORD,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
			.pmf_cfg = {
				.capable = true,
				.required = false
			},
		},
	};

	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
	    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
		    ESP_EVENT_ANY_ID,
		    &wifi_event_handler,
		    NULL,
		    &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
		    IP_EVENT_STA_GOT_IP,
		    &wifi_event_handler,
		    NULL,
		    &instance_got_ip));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
	ESP_ERROR_CHECK(esp_wifi_start() );

	ESP_LOGI(__func__, "wifi_init_sta finished.");

	/* Wait for the connection handler */
	bits = xEventGroupWaitBits(s_wifi_event_group,
	    WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
	    pdFALSE,
	    pdFALSE,
	    portMAX_DELAY);

	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(__func__, "connected to ap SSID:%s password:%s",
		    CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
	} else if (bits & WIFI_FAIL_BIT) {
		ESP_LOGI(__func__, "Failed to connect to SSID:%s, password:%s",
		    CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
		ret = ESP_ERR_WIFI_NOT_CONNECT;
	} else {
		ESP_LOGE(__func__, "UNEXPECTED EVENT");
		ret = ESP_ERR_WIFI_NOT_CONNECT;
	}

	/* The event will not be processed after unregister */
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT,
		    IP_EVENT_STA_GOT_IP, instance_got_ip));
	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT,
		    ESP_EVENT_ANY_ID, instance_any_id));
	vEventGroupDelete(s_wifi_event_group);	

	return ret;
}

/*
 * Generic handler to debug http response.
 */
esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
	switch(evt->event_id) {
		case HTTP_EVENT_ERROR:
			ESP_LOGI(__func__, "HTTP_EVENT_ERROR");
			break;
		case HTTP_EVENT_ON_CONNECTED:
			ESP_LOGI(__func__, "HTTP_EVENT_ON_CONNECTED");
			break;
		case HTTP_EVENT_HEADER_SENT:
			ESP_LOGI(__func__, "HTTP_EVENT_HEADER_SENT");
			break;
		case HTTP_EVENT_ON_HEADER:
			ESP_LOGI(__func__, "HTTP_EVENT_ON_HEADER");
			printf("%.*s", evt->data_len, (char*)evt->data);
			break;
		case HTTP_EVENT_ON_DATA:
			ESP_LOGI(__func__, "HTTP_EVENT_ON_DATA, len=%d",
			    evt->data_len);
			if (!esp_http_client_is_chunked_response(evt->client)) {
				printf("%.*s", evt->data_len, (char*)evt->data);
			}
			break;
		case HTTP_EVENT_ON_FINISH:
			ESP_LOGI(__func__, "HTTP_EVENT_ON_FINISH");
			break;
		case HTTP_EVENT_DISCONNECTED:
			ESP_LOGI(__func__, "HTTP_EVENT_DISCONNECTED");
			break;
	}
	return ESP_OK;
}

/*
 * Send the data to the influxdb server.
 */
static void send_data()
{
	esp_err_t err;
	esp_http_client_handle_t client;
	float temp = bmp280_ulp_get_temp();
	float pres = bmp280_ulp_get_pres();
	char * data = (char*)malloc(BUF_SIZE);

	esp_http_client_config_t config = {
		.url = INFLUX_URL,
		.event_handler = http_event_handler,
	};

	if(data == NULL) {
		return;
	}

	/* store the measurements and INFLUX_TAG in the buffer */
	snprintf(data, BUF_SIZE, INFLUX_TAG " temp=%0.2f\n" INFLUX_TAG
	    " pres=%0.2f\n", temp, pres);

	ESP_LOGI(__func__, "Influxdb url: %s\n", INFLUX_URL);
	ESP_LOGI(__func__, "Sent data:");
	printf("%s", data);

	client = esp_http_client_init(&config);
	esp_http_client_set_method(client, HTTP_METHOD_POST);
	esp_http_client_set_post_field(client, data, strlen(data));

	err = esp_http_client_perform(client);
	if (err == ESP_OK) {
		ESP_LOGI(__func__, "Status = %d, content_length = %lld",
		    esp_http_client_get_status_code(client),
		    esp_http_client_get_content_length(client));
	}

	esp_http_client_cleanup(client);

	free(data);
}

void app_main()
{
	esp_sleep_enable_timer_wakeup(((uint64_t)CONFIG_SAFE_TIMER * 1000000));

	if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
		bmp280_ulp_setup( NULL );
	} else {
		if (wifi_start() == ESP_OK) {
			send_data();
		}
	}

	bmp280_ulp_enable();

	ESP_LOGI(__func__, "Entering deep sleep\n");
	vTaskDelay(20);
	esp_deep_sleep_start();
}
