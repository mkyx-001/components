set(component_srcs "io_expander.c")

if(CONFIG_RUN_IO_EXPANDER_TEST)
    list(APPEND component_srcs "io_expander_test.c" "io_expander_test_runner.c")
else()
    list(APPEND component_srcs "main.c")
endif()

idf_component_register(
    SRCS ${component_srcs}
    PRIV_REQUIRES esp_io_expander
)
