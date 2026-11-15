#include "StarDataParadiseDLLler.h"

int main(void)
{
    sdp_buffer response = {0};
    sdp_error error = {0};
    const sdp_text empty_object = {"{}", 2};
    if (sdp_library_version() != 0x00000300u)
        return 1;
    if (sdp_dataset_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 2;
    if (sdp_model_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 3;
    if (sdp_backup_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 4;
    if (sdp_admin_command(0, 0, empty_object, &response, &error) != SDP_INVALID_ARGUMENT)
        return 5;
    return response.data || response.size || error.unknown_outcome ? 6 : 0;
}