#ifndef MOCE_APPLICATION_CONFIG_H
#define MOCE_APPLICATION_CONFIG_H
#include "bsp.h"
#define PROJECT_APP_PRIORITY 5
#define PROJECT_APP_STACK_BYTES 4096
BspStatus project_configure(void);
void project_run(void);
void application_main(void);
extern volatile BspStatus project_startup_status;
#endif
