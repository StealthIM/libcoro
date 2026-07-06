#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/*
 * wolfSSL 裸机配置 (mps2-an385 + QEMU, TLS 探针)。
 *
 * 基于官方 IDE/GCC-ARM/Header/user_settings.h 裁剪。目标: 在 Cortex-M3
 * 上以可接受耗时完成一次 TLS 握手。关键取舍:
 *   - SINGLE_THREADED: 裸机/探针无多线程 (真集成时 FreeRTOS 下会去掉)。
 *   - NO_FILESYSTEM: 证书走内存 buffer API (certs_test.h 的 DER)。
 *   - WOLFSSL_USER_IO: 自定义 IO 回调 (探针用 memory buffer 直连两端)。
 *   - 熵源: 自定义 CUSTOM_RAND_GENERATE_SEED (QEMU 无硬件 RNG)。
 *   - SP math + ECC: M3 上 ECC 比 RSA 快得多; 用 wolfSSL 的 SP (single
 *     precision) 数学, 针对 P-256 优化, 比通用 tfm 快。
 *   - 只留 TLS1.2 + ECDHE-ECDSA + AES-GCM + SHA256: 最小可用握手套件。
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WOLFSSL_GENERAL_ALIGNMENT   4
#define SINGLE_THREADED
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_USER_IO
/* WOLFSSL_USER_IO 只是不提供默认 IO 回调, 不阻止 wolfio.h 拉 <sys/socket.h>。
 * 探针用 memory IO, 无任何 socket, 加 WOLFSSL_NO_SOCK 彻底切断系统 socket 头。 */
#define WOLFSSL_NO_SOCK
#define SIZEOF_LONG_LONG            8

/* 证书/密钥走内存, 不用文件系统 */
#define NO_FILESYSTEM
#define NO_WRITEV

/* ---- 数学: SP (single precision), 针对 ECC P-256 优化 ----
 * WOLFSSL_SP_MATH_ALL: 通用 SP 数学 (任意曲线/位宽), 走 sp_int.c。
 * 不能和 WOLFSSL_SP_MATH 同开 (互斥, settings.h 会 #error)。 */
#define WOLFSSL_SP
#define WOLFSSL_SP_SMALL
#define WOLFSSL_HAVE_SP_ECC
#define WOLFSSL_SP_MATH_ALL
#define SP_WORD_SIZE           32   /* M3 是 32 位 */

/* ---- 公钥: 只 ECC (关掉 RSA/DH 省 flash + 握手快) ---- */
#define NO_RSA
#define NO_DH
#define HAVE_ECC
#define ECC_USER_CURVES
#define HAVE_ECC256            /* 只 P-256 */
#define ECC_TIMING_RESISTANT
#define HAVE_TLS_EXTENSIONS    /* ECDHE 的 supported_curves 扩展依赖它 */
#define HAVE_SUPPORTED_CURVES
#define HAVE_ECC_KEY_IMPORT

/* ---- SNI + IP-SAN 校验 (async_ssl client hostname 校验用) ---- */
#define HAVE_SNI               /* wolfSSL_UseSNI / WOLFSSL_SNI_HOST_NAME */
#define WOLFSSL_IP_ALT_NAME    /* wolfSSL_check_ip_address (证书 IP:SAN) */
#define KEEP_PEER_CERT         /* check_domain_name/ip 需要留着对端证书 */

/* ---- TLS 版本: 1.2 + 1.3 都留 (别用 #define WOLFSSL_NO_TLS12 0, 那样
 * 仍算"已定义"会关掉 1.2)。NO_OLD_TLS 去掉 <1.2。探针用 1.3。 ---- */
#define WOLFSSL_TLS13
#define NO_OLD_TLS

/* ---- 对称/摘要: AES-GCM + SHA256 (ECDHE-ECDSA-AES128-GCM-SHA256) ---- */
#define HAVE_AESGCM
#define WOLFSSL_SHA256
#define HAVE_HKDF
#define WC_NO_HARDEN           /* 探针: 关掉 blinding 等加速; 真部署再权衡 */

/* 去掉用不到的算法, 省 flash/RAM */
#define NO_DES3
#define NO_DSA
#define NO_RC4
#define NO_MD4
#define NO_MD5                 /* TLS1.3 不需要 MD5; hmac.c 否则引用 wc_Md5* */
/* SHA-1 保留: TLS1.3 本身不用, 但 asyncweb 的 anet_tls_sha1 (WS 握手 RFC6455
 * 强制 SHA-1) 需要。嵌入式后端也该支持 WS。 */
#define WOLFSSL_SHA
#define NO_PWDBASED
#define NO_PSK

/* ---- 熵源: QEMU 无硬件 RNG, 自己提供 seed 生成 ---- */
#define CUSTOM_RAND_GENERATE_SEED  wolf_gen_seed
extern int wolf_gen_seed(unsigned char* output, unsigned int sz);

/* ---- 时间: 用 newlib 的 time.h (别自定义 time_t/struct tm, 会和 newlib
 * 冲突)。newlib 的 time() 在裸机走 rdimon semihosting SYS_TIME; 若不准,
 * wolfSSL 证书有效期校验可能失败——探针里用 NO_ASN_TIME 关掉时间校验以
 * 隔离"公钥数学能否跑通"这个真问题 (时间校验是另一回事)。 ---- */
#define NO_ASN_TIME

/* 调试: 探针关掉 (开了输出太多) */
/* #define DEBUG_WOLFSSL */

#ifdef __cplusplus
}
#endif

#endif /* WOLFSSL_USER_SETTINGS_H */
