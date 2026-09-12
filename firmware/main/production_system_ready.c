#include "esp_log.h"

#include "app/result.h"
#include "hal/boards/board_composition.h"
#include "hal/outputs/output_driver.h"

static const char *TAG = "irrigation_controller";

void app_main(void)
{
    OutputDriver *output_driver = board_output_driver();
    if (output_driver_initialize_safe_off(output_driver) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "production output initialization failed; relay outputs remain unavailable");
        return;
    }

    ESP_LOGI(TAG, "production output driver ready; all relay outputs are OFF");
}
