/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright © 2019-2022 Tensil AI Company */

#include <malloc.h>
#include <stdio.h>

#include "ff.h"

#include "tensil/dram.h"
#include "tensil/driver.h"
#include "tensil/instruction.h"
#include "tensil/model.h"
#include "tensil/tcu.h"

#include "console.h"
#include "stopwatch.h"

#define MODEL_FILE_PATH "baseline/resnet20v2_cifar_onnx_zcu104.tmodel"

static size_t argmax(size_t size, const float *buffer) {
    if (!size)
        return -1;

    float max = buffer[0];
    size_t max_i = 0;

    for (size_t i = 1; i < size; i++)
        if (buffer[i] > max) {
            max = buffer[i];
            max_i = i;
        }

    return max_i;
}

#define CHANNEL_TO_FLOAT(v) ((float)v / 255.0)

static float channel_mean(size_t size, const u8 *buffer) {
    float sum = 0.0;
    for (size_t i = 0; i < size; i++)
        sum += CHANNEL_TO_FLOAT(buffer[i]);

    return sum / (float)size;
}

#define CIFAR_PIXELS_SIZE 1024
#define CIFAR_CLASSES_SIZE 10

#define CIFAR_BUFFER_BASE TENSIL_PLATFORM_DRAM_BUFFER_HIGH

static const char *cifar_classes[] = {
    "airplane", "automobile", "bird",  "cat",  "deer",
    "dog",      "frog",       "horse", "ship", "truck",
};

static const char progress[] = {'-', '\\', '|', '/'};

static tensil_error_t test_resnet20v2_on_cifar(struct tensil_driver *driver,
                                               const struct tensil_model *model,
                                               const char *file_name,
                                               bool print_images) {
    FIL fil;
    FILINFO fno;
    UINT bytes_read;
    tensil_error_t error = TENSIL_ERROR_NONE;

    FRESULT res = f_stat(file_name, &fno);
    if (res)
        return TENSIL_FS_ERROR(res);

    res = f_open(&fil, file_name, FA_READ);

    if (res)
        return TENSIL_FS_ERROR(res);

    printf("Reading CIFAR test images from %s...\n", file_name);

    res = f_read(&fil, (void *)CIFAR_BUFFER_BASE, fno.fsize, &bytes_read);
    f_close(&fil);

    if (res)
        return TENSIL_FS_ERROR(res);

    size_t total_count = fno.fsize / (CIFAR_PIXELS_SIZE * 3 + 1);
    size_t misclass_count = 0;
    u8 *ptr = (u8 *)CIFAR_BUFFER_BASE;

    printf("Testing ResNet20V2 on CIFAR...\n");

    float total_seconds = 0;

    if (print_images)
        console_clear_screen();

    for (size_t i = 0; i < total_count; i++) {
        size_t expected_class = *ptr;
        ptr += 1;

        u8 *red = ptr;
        ptr += CIFAR_PIXELS_SIZE;

        u8 *green = ptr;
        ptr += CIFAR_PIXELS_SIZE;

        u8 *blue = ptr;
        ptr += CIFAR_PIXELS_SIZE;

        float red_mean = channel_mean(CIFAR_PIXELS_SIZE, red);
        float green_mean = channel_mean(CIFAR_PIXELS_SIZE, green);
        float blue_mean = channel_mean(CIFAR_PIXELS_SIZE, blue);

        for (size_t j = 0; j < CIFAR_PIXELS_SIZE; j++) {
            float pixel[] = {CHANNEL_TO_FLOAT(red[j]) - red_mean,
                             CHANNEL_TO_FLOAT(green[j]) - green_mean,
                             CHANNEL_TO_FLOAT(blue[j]) - blue_mean};

            error = tensil_driver_load_model_input_vector_scalars(
                driver, model, "x:0", j, 3, pixel);

            if (error)
                goto cleanup;
        }

        struct stopwatch sw;
        error = stopwatch_start(&sw);

        if (error)
            goto cleanup;

        error = tensil_driver_run(driver, NULL);

        if (error)
            goto cleanup;

        stopwatch_stop(&sw);
        float seconds = stopwatch_elapsed_seconds(&sw);

        total_seconds += seconds;

        float result[CIFAR_CLASSES_SIZE];
        error = tensil_driver_get_model_output_scalars(
            driver, model, "Identity:0", CIFAR_CLASSES_SIZE, result);

        if (error)
            goto cleanup;

        size_t actual_class = argmax(CIFAR_CLASSES_SIZE, result);

        if (actual_class != expected_class)
            misclass_count++;

        if (print_images) {
            console_set_cursor_position(1, 1);
            printf("%06zu: %.2f fps %c\n", i, (float)1 / seconds,
                   progress[i % 4]);

            if (i % 100 == 0) {
                printf("\nImage:");

                for (size_t j = 0; j < CIFAR_PIXELS_SIZE; j++) {
                    console_set_background_color(red[j], green[j], blue[j]);

                    if (j % 32 == 0)
                        printf("\n");

                    printf("  ");
                }

                printf("\n");
                console_reset_background_color();

                printf("\nResult:\n");

                error = tensil_driver_print_model_output_vectors(driver, model,
                                                                 "Identity:0");

                if (error)
                    goto cleanup;

                if (actual_class == expected_class)
                    console_set_foreground_color(0, 255, 0);
                else
                    console_set_foreground_color(255, 0, 0);

                printf(
                    "CIFAR expected class = %s, actual class = %s         \n",
                    cifar_classes[expected_class], cifar_classes[actual_class]);

                console_reset_foreground_color();
            }
        }
    }

cleanup:
    if (print_images) {
        console_clear_screen();
        console_set_cursor_position(1, 1);
    }

    if (error == TENSIL_ERROR_NONE)
        printf("ResNet20V2 on CIFAR: %lu images %.2f accuracy at %.2f fps\n",
               total_count, (1.0 - (float)misclass_count / (float)total_count),
               (float)total_count / total_seconds);

    return error;
}

static FATFS fatfs;

int main() {
    tensil_error_t error = TENSIL_ERROR_NONE;
    FRESULT res;
    res = f_mount(&fatfs, "0:/", 0);

    if (res) {
        error = TENSIL_FS_ERROR(res);
        goto cleanup;
    }

    struct tensil_driver driver;
    error = tensil_driver_init(&driver);

    if (error)
        goto cleanup;

    struct tensil_model resnet20v2_model;
    error = tensil_model_from_file(&resnet20v2_model,
                                   MODEL_FILE_PATH);

    if (error)
        goto cleanup;

    error = tensil_driver_load_model(&driver, &resnet20v2_model);

    if (error)
        goto cleanup;

    error = test_resnet20v2_on_cifar(&driver, &resnet20v2_model,
                                     "test_batch.bin", true);

    if (error)
        goto cleanup;

cleanup:
    if (error)
        tensil_error_print(error);

    return 0;
}
