/*
 * SDRIAK additions to the Mbed-TLS configuration.
 *
 * Included at the end of mbedtls/build_info.h via MBEDTLS_USER_CONFIG_FILE, so
 * everything here overrides the stock mbedtls_config.h defaults.
 *
 * Android: the platform entropy source must not block.
 *
 * mbedtls_platform_entropy_poll() prefers the getrandom() system call, but it
 * is only compiled in when __GLIBC__ is defined (see library/entropy_poll.c).
 * Bionic is not glibc, so on Android the call is compiled out and the stock
 * configuration falls back to reading MBEDTLS_PLATFORM_DEV_RANDOM, which
 * defaults to "/dev/random". On kernels before 5.6 that file blocks until the
 * entropy pool is considered initialised, which on an emulator — or on any
 * device that has just booted without a hardware RNG — can take effectively
 * forever.
 *
 * SDRIAK calls curl_global_init() (which seeds mbedTLS's CTR_DRBG through
 * psa_crypto_init()) on the main thread before the window appears, so that
 * blocking read used to hang startup with a black screen and no log output
 * past "SDRIAK vX.Y.Z".
 *
 * /dev/urandom never blocks. It is backed by the same kernel CSPRNG; the only
 * caveat is that early after boot it may return bytes generated before the
 * pool was fully seeded, which does not apply to an app the user starts by
 * hand. This is the choice the mbedtls_config.h documentation recommends for
 * exactly this situation.
 */
#pragma once

#if defined(__ANDROID__) && !defined(MBEDTLS_PLATFORM_DEV_RANDOM)
#define MBEDTLS_PLATFORM_DEV_RANDOM "/dev/urandom"
#endif
