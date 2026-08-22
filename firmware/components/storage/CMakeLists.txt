idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES state_store fatfs wear_levelling
)
