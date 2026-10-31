# components/idf_fixes/check_idf_lock.cmake —— T9.47 的升级绊线：IDF 的 ECDSA 锁已经连带 SHA/AES 锁（上游 ba13c2852a）就停
#
# 两种用法：
#   CMakeLists.txt 里 include()，构建期每次都查当前 IDF；
#   VERIFY.sh 里 cmake -DIDF_LOCK_SRC=<文件> -P check_idf_lock.cmake，拿改过的样本证明它真会报错。
# 函数体按"行首的 }"截（esp_crypto_lock.c 的函数都这样结尾），不用 [^}]*：函数体里有 if (...) { } 块，
# 遇到第一个 } 就停会漏看后面的行 —— 漏判的代价是重复加锁、第一次验签就锁死（审计 ⚠️，2026-10-23）。
function(idf_fixes_check_ecdsa_lock src)
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "idf_fixes: 找不到 ${src}，无法确认 IDF 是否已带 ECDSA 锁修复（T9.47）")
    endif()
    file(READ "${src}" text)
    set(sig "void esp_crypto_ecdsa_lock_acquire(void)")
    string(FIND "${text}" "${sig}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "idf_fixes: ${src} 里认不出 esp_crypto_ecdsa_lock_acquire，先人工核对上游再改这里（T9.47）")
    endif()
    string(SUBSTRING "${text}" ${at} -1 rest)
    string(FIND "${rest}" "\n}" end)
    if(end EQUAL -1)
        message(FATAL_ERROR "idf_fixes: ${src} 里 esp_crypto_ecdsa_lock_acquire 没有行首的 }，先人工核对（T9.47）")
    endif()
    string(SUBSTRING "${rest}" 0 ${end} body)
    if(body MATCHES "esp_crypto_sha_aes_lock_acquire")
        message(FATAL_ERROR "idf_fixes: 这版 IDF 的 ECDSA 锁已连带 SHA/AES 锁（上游已修），"
                            "删掉 components/idf_fixes 的 ECDSA 锁 wrap，否则重复加锁会锁死（T9.47）")
    endif()
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND DEFINED IDF_LOCK_SRC)
    idf_fixes_check_ecdsa_lock("${IDF_LOCK_SRC}")
endif()
