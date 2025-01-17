// RATE LIMIT ON API: 60 requests per hour.
// https://pvoutput.org/help/api_specification.html#rate-limits

#include <esp_rmaker_utils.h>

#include <string.h>

#include "sdkconfig.h"
#include "app_pvoutput_org.h"
#include "app_wifi.h"
#include "app_time.h"
#include "app_modbus.h"

#include <esp_err.h>

#define MAX_HTTP_RECV_BUFFER    512
#define MAX_HTTP_OUTPUT_BUFFER  8192

static const char *TAG = "app_pvoutput";

#define PVOUTPUT_ADD_STATUS_STR_MAX_LEN 60

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        #if ESP_IDF_VERSION_MAJOR >= 5
        case HTTP_EVENT_REDIRECT:
            break;
        #endif
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER");
            printf("%.*s", evt->data_len, (char*)evt->data);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                printf("%.*s", evt->data_len, (char*)evt->data);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

void build_pvoutput_query_string(char *buffer, char* date, char* time, float watts, float volts) {
   
   // i.e: addstatus.jsp?d=20220328&t=21:56&v2=1&v6=230

   snprintf(buffer,
            PVOUTPUT_ADD_STATUS_STR_MAX_LEN,
            "d=%s&t=%s&v2=%f&v6=%f",
            date,
            time,
            watts, 
            volts);
}

void print_current_datetime() {
    ESP_LOGI(TAG, "Current date is: %s", get_pvoutput_fmt_date()); 
    ESP_LOGI(TAG, "Current time is: %s", get_pvoutput_fmt_time());
}

int app_pvoutput_init() {
    // Block here if SNTP is not set. Things will not work properly if system time is not set properly
    // i.e:
    // PVOutput does not allow status data submissions older than 14 days...
    // ... and 1970 happened a long time ago ;)
    //
    // Wait forever because we don't have good timeseries without time sync
    esp_rmaker_time_wait_for_sync(portMAX_DELAY);

    xTaskCreate(pvoutput_update, "pvoutput_task", 16384, NULL, 5, &pvoutput_task);
    
    // The task above should never end, if it does, that's a fail :-!
    return ESP_FAIL;
}

// void sntp_set_time() {
//     sntp_setoperatingmode(SNTP_OPMODE_POLL);
//     sntp_setservername(0, "pt.pool.ntp.org");
//     sntp_setservername(1, "pool.ntp.org");
//     sntp_init();
// }

void pvoutput_update()
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    char local_pvoutput_query_string[PVOUTPUT_ADD_STATUS_STR_MAX_LEN];
    
    //uint32_t ulNotifiedValue;
    float volts; // Single phase reading as each of three phases is equal
    float watts;

    while(1) {
        //xTaskNotifyWait(0, ULONG_MAX, &ulNotifiedValue, pdMS_TO_TICKS(1000*60*2));
        ESP_LOGI(TAG, "PVoutput task waits for data to be produced...\n");
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG, "PVoutput task wakes up...\n");
            // XXX: Find a better way to generalise obscure Watts/Volts index in mb_readings
            // across power meter readers
            // watts = mb_readings[4].value;
            // volts = mb_readings[6].value;
            watts = g_watts;
            volts = g_volts;

            build_pvoutput_query_string( local_pvoutput_query_string,
                                        get_pvoutput_fmt_date(),
                                        get_pvoutput_fmt_time(),
                                        watts,
                                        volts);

            esp_http_client_config_t config = {
                .host = "pvoutput.org",
                .path = "/service/r2/addstatus.jsp",
                .method = HTTP_METHOD_POST,
                .query = local_pvoutput_query_string,
                .event_handler = _http_event_handler,
                .user_data = local_response_buffer,        // Pass address of local buffer to get response
                .disable_auto_redirect = true,
                //.use_global_ca_store = true,
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);

            esp_http_client_set_header(client, "Content-Type", "application/json");
            // XXX: Do not setup key if it starts with "<" (not setup)
            esp_http_client_set_header(client, "X-Pvoutput-Apikey", CONFIG_PVOUTPUT_ORG_API_KEY);
            esp_http_client_set_header(client, "X-Pvoutput-SystemId", CONFIG_PVOUTPUT_ORG_SYSTEM_ID);

            ESP_LOGI(TAG, "Sending power data to: https://%s%s%s", config.host, config.path, local_pvoutput_query_string);
            #if CONFIG_PROD_MODE
                esp_err_t err = esp_http_client_perform(client);
                if (err == ESP_OK) {
                    #if ESP_IDF_VERSION_MAJOR >= 5
                    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %"PRIx64"",
                    #else
                    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %x",
                    #endif
                            esp_http_client_get_status_code(client),
                            esp_http_client_get_content_length(client));
                } else {
                    ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
                }
            #endif

            // Cleanup, otherwise after ~8 open & dead connections we exhaust memory.
            esp_http_client_cleanup(client);

            // Done with reading modbus values and reporting them
            //ESP_LOGI(TAG, "Notifying modbus_task that we are done with one pvoutput data submission...\n");
            xTaskNotifyGive(modbus_task);

            // if (modbus_task != NULL) {
            //     xTaskNotify(modbus_task, 0, eSetValueWithoutOverwrite);
            // } else {
            //     printf("NULL pointer\n");
            //     fflush(stdout);
            // }
        } else {
            ESP_LOGI(TAG, "PVOutput oops... I should be handling errors here\n");
        }
    }
}
