#include "StarDataParadiseDLLler.h"

int main(void)
{
    return sdp_library_version() == 0x00000100u ? 0 : 1;
}
