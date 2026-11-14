# Application chooses the hardware composition before compiling Board users.
if(NOT DEFINED MOCE_SHEILD OR NOT MOCE_SHEILD STREQUAL "none")
    message(FATAL_ERROR "This Board currently accepts only explicit MOCE_SHEILD=none")
endif()
set(MOCE_BOARD_SELECTION_DEFINE MOCE_SHEILD_NONE=1)
