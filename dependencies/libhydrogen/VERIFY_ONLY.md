# Verification-Only Build Configuration

This document explains how to configure libhydrogen for verification-only mode, which removes all signing and key generation functionality to reduce code size.

## Usage

Define `HYDRO_SIGN_VERIFY_ONLY` to `1` before including `libhydrogen.h`:

```c
#define HYDRO_SIGN_VERIFY_ONLY 1
#include "libhydrogen.h"
```

Or compile with the flag:

```bash
gcc -DHYDRO_SIGN_VERIFY_ONLY=1 -c libhydrogen.c
```

## What Gets Removed

When `HYDRO_SIGN_VERIFY_ONLY` is enabled, the following are automatically disabled:

### Signing Functions (Removed)
- `hydro_sign_keygen()` - Key generation
- `hydro_sign_keygen_deterministic()` - Deterministic key generation
- `hydro_sign_create()` - Signature creation
- `hydro_sign_final_create()` - Finalize signature creation

### Internal Functions (Removed)
- `hydro_sign_p2()` - Internal signing function
- `hydro_sign_prehash()` - Internal signing function
- `hydro_x25519_sc_montmul()` - Scalar multiplication (only used for signing)
- `hydro_x25519_scalarmult_base_uniform()` - Base point multiplication (only used for signing/keygen)

### Entire Modules (Automatically Disabled)
Since verification only needs hash and X25519 operations, the following modules are **automatically disabled** when `HYDRO_SIGN_VERIFY_ONLY` is set:
- **Random number generation** - Not needed (verification uses no randomness)
- **KDF (Key Derivation)** - Not needed
- **Secretbox (Encryption)** - Not needed
- **KX (Key Exchange)** - Not needed
- **PWHASH (Password Hashing)** - Not needed
- **Hash keygen** - Not needed (verification uses NULL keys)

You can still explicitly enable any of these modules if needed by defining their disable flags to 0 before including `libhydrogen.h`.

## What Remains Available

All verification functions remain available:

- `hydro_sign_verify()` - Verify a signature
- `hydro_sign_final_verify()` - Verify a signature (streaming)
- `hydro_sign_init()` - Initialize hash state
- `hydro_sign_update()` - Update hash state

## Overriding Automatic Disabling

When `HYDRO_SIGN_VERIFY_ONLY` is enabled, unused modules are automatically disabled. If you need to keep a specific module enabled (e.g., for other parts of your application), you can override this by explicitly setting the disable flag to 0:

```c
#define HYDRO_SIGN_VERIFY_ONLY 1
#define HYDRO_DISABLE_RANDOM 0      // Keep random enabled (overrides auto-disable)
#include "libhydrogen.h"
```

## Manual Feature Flags

You can also manually disable modules without enabling verify-only mode:

```c
#define HYDRO_DISABLE_KX 1          // Disable key exchange
#define HYDRO_DISABLE_SECRETBOX 1    // Disable secretbox encryption
#define HYDRO_DISABLE_KDF 1          // Disable key derivation
#define HYDRO_DISABLE_PWHASH 1       // Disable password hashing
#define HYDRO_DISABLE_RANDOM 1       // Disable random (only if all features using it are disabled)
#include "libhydrogen.h"
```

## Example

```c
#define HYDRO_SIGN_VERIFY_ONLY 1
#include "libhydrogen.h"
#include <stdio.h>

int main(void) {
    hydro_init();
    
    // Public key and signature (obtained from signer)
    uint8_t pk[hydro_sign_PUBLICKEYBYTES] = { /* ... */ };
    uint8_t sig[hydro_sign_BYTES] = { /* ... */ };
    const char *msg = "Hello, world!";
    
    // Verify signature
    if (hydro_sign_verify(sig, msg, strlen(msg), "example", pk) == 0) {
        printf("Signature valid!\n");
    } else {
        printf("Signature invalid!\n");
    }
    
    return 0;
}
```

## Size Reduction

The exact size reduction depends on your compiler and optimization settings, but you can expect:
- **30-50% reduction** in code size when only verification is enabled
- **Additional 20-30% reduction** if other unused modules are also disabled

Use your compiler's size analysis tools to measure the exact reduction for your use case.

