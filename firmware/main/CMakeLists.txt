idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES obd_driver pid_engine state_store storage ui
)
