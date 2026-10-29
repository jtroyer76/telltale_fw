idf_component_register(
    SRC_DIRS "." "board" "fonts"
    INCLUDE_DIRS "." "board" "fonts"
    EMBED_TXTFILES "../data/default_config.json")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
