# UTF-8 校验与修复（T9.24 / T9.32 / T9.33）。只依赖 libc：
# webui 的 JSON 输出与 config_mgr 的写入口、导出共用这一份判据。
idf_component_register(
    SRCS "utf8.c"
    INCLUDE_DIRS "include"
)
