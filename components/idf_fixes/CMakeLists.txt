# components/idf_fixes —— 本地回合的 IDF 上游修复（升级到带修复的 IDF 就整段删掉）
#
# crypto_reset_lock.c（T9.47）：C5 上 ECDSA 的复位连带复位 RSA 与 SHA，v6.0.2 的 ECDSA 锁却不连带它们的锁
#   → 回合上游 release/v6.0 的 ba13c2852a + 48a864dd75（v6.0.3 不含）。理由、证据与删除条件见那个文件的头注。
idf_component_register(
    SRCS "crypto_reset_lock.c"
         "crypto_race_diag.c"    # 诊断构建专用（CONFIG_BITENGINE_DIAG_CRYPTO_RACE），出厂编出来是空的
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_security soc mbedtls esp_timer freertos log
    WHOLE_ARCHIVE                # 没人引用 __wrap_ 所在的目标文件之前，静态库不会把它拉进来
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_crypto_ecdsa_lock_acquire"
    "-Wl,--wrap=esp_crypto_ecdsa_lock_release")

# 绊线：上游第一个提交（ba13c2852a）让 ECDSA 锁无条件连带 SHA/AES 锁。IDF 已经这样了还套这一层，
# 会对不可重入的锁重复加锁、第一次 ECDSA 验签就锁死整机（第二个提交由 crypto_reset_lock.c 的 #error 绊住）。
include(${CMAKE_CURRENT_LIST_DIR}/check_idf_lock.cmake)
idf_build_get_property(_idf_path IDF_PATH)
idf_fixes_check_ecdsa_lock("${_idf_path}/components/esp_security/src/esp_crypto_lock.c")
