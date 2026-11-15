#include "StarDataParadiseDLLler.h"

int main(void)
{
    return sdp_library_version() == 0x00000200u ? 0 : 1;
}
