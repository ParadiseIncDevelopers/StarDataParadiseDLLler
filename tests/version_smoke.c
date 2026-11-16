#include "StarDataParadiseDLLler.h"

#include <stdio.h>
#include <string.h>

static int scheduled_recipe_validation(void)
{
    const char* sql = "{\"kind\":\"sql_script\",\"sql\":[\"SELECT 1\"],"
        "\"rule\":{\"every_seconds\":60},\"request_token\":\"00000000000000000000000000000001\"}";
    const char* retrain = "{\"kind\":\"retrain\",\"name\":\"public.model\","
        "\"dataset_id\":\"00000000000000000000000000000002\",\"seed\":4294967295,\"options\":{},"
        "\"rule\":{\"every_seconds\":60},\"request_token\":\"00000000000000000000000000000001\"}";
    const struct {
        sdp_job_operation operation;
        const char* body;
        sdp_status expected;
    } cases[] = {
        {SDP_JOB_SCHEDULE_CREATE, sql, SDP_INVALID_STATE},
        {SDP_JOB_SCHEDULE_CREATE, retrain, SDP_INVALID_STATE},
        {SDP_JOB_SUBMIT, sql, SDP_INVALID_ARGUMENT},
        {SDP_JOB_SUBMIT, retrain, SDP_INVALID_ARGUMENT},
        {SDP_JOB_SCHEDULE_CREATE, "{\"kind\":\"sql_script\",\"sql\":\"SELECT 1\","
            "\"rule\":{\"every_seconds\":60},\"request_token\":\"00000000000000000000000000000001\"}",
            SDP_INVALID_ARGUMENT},
        {SDP_JOB_SCHEDULE_CREATE, "{\"kind\":\"sql_script\",\"sql\":[\"SELECT 1\\u0000\"],"
            "\"rule\":{\"every_seconds\":60},\"request_token\":\"00000000000000000000000000000001\"}",
            SDP_INVALID_ARGUMENT},
        {SDP_JOB_SCHEDULE_CREATE, "{\"kind\":\"retrain\",\"name\":\"public.model\","
            "\"dataset_id\":\"00000000000000000000000000000002\",\"seed\":4294967296,"
            "\"rule\":{\"every_seconds\":60},\"request_token\":\"00000000000000000000000000000001\"}",
            SDP_INVALID_ARGUMENT},
        {SDP_JOB_SCHEDULE_CREATE, "{\"kind\":\"retrain\",\"name\":\"public.model\","
            "\"dataset_id\":\"00000000000000000000000000000002\",\"pause_after_epoch\":1,"
            "\"rule\":{\"every_seconds\":60},\"request_token\":\"00000000000000000000000000000001\"}",
            SDP_INVALID_ARGUMENT},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
    {
        sdp_buffer response = {0};
        sdp_error error = {0};
        const sdp_text payload = {cases[i].body, (uint32_t)strlen(cases[i].body)};
        const sdp_status status = sdp_job_command(0, cases[i].operation, payload, &response, &error);
        if (status != cases[i].expected || response.data || response.size || error.unknown_outcome)
        {
            fprintf(stderr, "Scheduled recipe case %zu returned %u (expected %u)\n", i, status, cases[i].expected);
            sdp_buffer_free(&response);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    sdp_buffer response = {0};
    sdp_error error = {0};
    const sdp_text empty_object = {"{}", 2};
    if (sdp_library_version() != 0x00000401u)
        return 1;
    if (sdp_dataset_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 2;
    if (sdp_model_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 3;
    if (sdp_backup_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 4;
    if (sdp_admin_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 5;
    if (sdp_get_session_info(0, &response, &error) != SDP_INVALID_STATE)
        return 6;
    if (response.data || response.size || error.unknown_outcome)
        return 7;
    return scheduled_recipe_validation() ? 0 : 8;
}
