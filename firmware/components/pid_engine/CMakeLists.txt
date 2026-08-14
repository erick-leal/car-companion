idf_component_register(
    SRCS "pid_engine.c" "pid_math.c"
    INCLUDE_DIRS "include"
    REQUIRES state_store obd_driver
)
