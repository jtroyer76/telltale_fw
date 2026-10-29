idf_component_register(
    SRC_DIRS "." "board" "fonts"
    INCLUDE_DIRS "." "board" "fonts"
    EMBED_TXTFILES "../data/default_config.json")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
# LVGL's heap in PSRAM (lv_mem_psram.h); free() works on any heap
target_include_directories(${lvgl_lib} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(${lvgl_lib} PRIVATE LV_MEM_CUSTOM_ALLOC=lv_psram_alloc LV_MEM_CUSTOM_REALLOC=lv_psram_realloc)
