#include "config.h"
#include "board_platform.h"
#include "bsp_ecw6700_io_config.h"
/* defconfig selects SDK CPU160/AHB80/APB40. Called in a running SDK task.
 * Empty IO composition enables no module, radio worker or protocol port. */
static Ecw6700IoStorage io;
BspStatus project_configure(void)
{
    BspStatus status=bsp_ecw6700_io_compose(board_get_platform_descriptor(), NULL, 0, &io);
    if(status!=BSP_OK) return status;
    const Ecw6700PlatformConfig platform={.board=&io.board};
    return bsp_platform_init(&platform);
}
void project_run(void)
{
    project_startup_status=project_configure();
    if(project_startup_status==BSP_OK) application_main();
}
