#ifndef BOTAN_BUILD_INFO_H_
#define BOTAN_BUILD_INFO_H_

/**
* @file  build.h
* @brief Build configuration for Botan 3.9.0
*/

/* NOLINTBEGIN(*-macro-usage,*-macro-to-enum) */

/**
 * @defgroup buildinfo Build Information
 */

/**
 * @ingroup buildinfo
 * @defgroup buildinfo_version Build version information
 * @{
 */

/**
* The major version of the release
*/
#define BOTAN_VERSION_MAJOR 3

/**
* The minor version of the release
*/
#define BOTAN_VERSION_MINOR 9

/**
* The patch version of the release
*/
#define BOTAN_VERSION_PATCH 0

/**
 * Expands to an integer of the form YYYYMMDD if this is an official
 * release, or 0 otherwise. For instance, 2.19.0, which was released
 * on January 19, 2022, has a `BOTAN_VERSION_DATESTAMP` of 20220119.
 *
 * This macro is deprecated; use version_datestamp from version.h
 *
 * TODO(Botan4) remove this
 */
#define BOTAN_VERSION_DATESTAMP 20250805

/**
 * A string set to the release type
 *
 * This macro is deprecated
 *
 * TODO(Botan4) remove this
 */
#define BOTAN_VERSION_RELEASE_TYPE "release"

/**
 * A macro expanding to a string that is set to a revision identifier
 * corresponding to the source, or "unknown" if this could not be
 * determined. It is set for all official releases.
 *
 * This macro is deprecated; use version_vc_revision from version.h
 *
 * TODO(Botan4) remove this
 */
#define BOTAN_VERSION_VC_REVISION "git:07e1cfe0a06b224bbb37ad534736924931184246"

/**
 * A macro expanding to a string that is set at build time using the
 * `--distribution-info` option. It allows a packager of the library
 * to specify any distribution-specific patches. If no value is given
 * at build time, the value is the string "unspecified".
 *
 * This macro is deprecated; use version_distribution_info from version.h
 *
 * TODO(Botan4) remove this
 */
#define BOTAN_DISTRIBUTION_INFO "unspecified"

/**
 * @}
 */

/**
 * @ingroup buildinfo
 * @defgroup buildinfo_configuration Build configurations
 * @{
 */




#ifndef BOTAN_DLL
  #define BOTAN_DLL 
#endif

/* Target identification and feature test macros */

#define BOTAN_TARGET_OS_HAS_FILESYSTEM
#define BOTAN_TARGET_OS_HAS_THREADS



/**
 * @}
 */

/**
 * @ingroup buildinfo
 * @defgroup buildinfo_modules Enabled modules and API versions
 * @{
 */

/*
* Module availability definitions
*/
#define BOTAN_HAS_AES 20131128
#define BOTAN_HAS_ASN1 20201106
#define BOTAN_HAS_AUTO_RNG 20161126
#define BOTAN_HAS_AUTO_SEEDING_RNG 20160821
#define BOTAN_HAS_BASE64_CODEC 20131128
#define BOTAN_HAS_BIGINT 20240529
#define BOTAN_HAS_BLOCK_CIPHER 20131128
#define BOTAN_HAS_CIPHER_MODES 20180124
#define BOTAN_HAS_CIPHER_MODE_PADDING 20131128
#define BOTAN_HAS_EME_PKCS1 20190426
#define BOTAN_HAS_EME_PKCS1v15 20131128
#define BOTAN_HAS_EMSA_PKCS1 20140118
#define BOTAN_HAS_HASH 20180112
#define BOTAN_HAS_HEX_CODEC 20131128
#define BOTAN_HAS_HKDF 20170927
#define BOTAN_HAS_HMAC 20131128
#define BOTAN_HAS_HMAC_DRBG 20140319
#define BOTAN_HAS_KDF 20250528
#define BOTAN_HAS_KDF_BASE 20131128
#define BOTAN_HAS_MAC 20150626
#define BOTAN_HAS_MODES 20150626
#define BOTAN_HAS_MODE_CBC 20131128
#define BOTAN_HAS_NUMBERTHEORY 20201108
#define BOTAN_HAS_OCSP 20201106
#define BOTAN_HAS_PEM_CODEC 20131128
#define BOTAN_HAS_PKCSV15_ENCRYPTION_PADDING 20250126
#define BOTAN_HAS_PKCSV15_SIGNATURE_PADDING 20250126
#define BOTAN_HAS_PK_PADDING 20131128
#define BOTAN_HAS_PUBLIC_KEY_CRYPTO 20131128
#define BOTAN_HAS_RSA 20160730
#define BOTAN_HAS_RSA_SIGNATURE_PADDING 20250720
#define BOTAN_HAS_SHA1 20131128
#define BOTAN_HAS_SHA2_32 20131128
#define BOTAN_HAS_SHA_224 20250130
#define BOTAN_HAS_SHA_256 20250130
#define BOTAN_HAS_STATEFUL_RNG 20160819
#define BOTAN_HAS_SYSTEM_RNG 20141202
#define BOTAN_HAS_X509 20201106
#define BOTAN_HAS_X509_CERTIFICATES 20201106


/*
* Internal module feature definitions
*
* These macros have been in the past visible in build.h as feature macros
* but in the future these will be only visible in an internal header.
* Applications should not rely on or check for these macros.
*/
#define BOTAN_HAS_BIGINT_MP 20151225
#define BOTAN_HAS_DYNAMIC_LOADER 20160310
#define BOTAN_HAS_HASH_ID 20131128
#define BOTAN_HAS_MDX_HASH_FUNCTION 20131128
#define BOTAN_HAS_OS_UTILS 20241202


/**
 * @}
 */

/* NOLINTEND(*-macro-usage,*-macro-to-enum) */

#endif
