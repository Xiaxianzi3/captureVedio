#include <stdio.h>
#include "video.h"

int main()
{
    getVideo("/dev/video0", 100, "output.mp4");
    return 0;
}