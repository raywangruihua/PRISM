// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 */

#include <tee_internal_api.h>
#include <tee_internal_api_extensions.h>

#include <prism_ta.h>

// AES-128-GCM parameters
#define AES_KEY_BITS   128
#define AES_KEY_BYTES  16
#define GCM_IV_LEN     12 // 96-bit nonce, prepended by the CA
#define GCM_TAG_LEN    16 // 128-bit tag, in BYTES
#define GCM_TAG_BITS   (GCM_TAG_LEN * 8)

// Hardcoded key for demonstration only
static const uint8_t g_aes_key[AES_KEY_BYTES] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

TEE_Result TA_CreateEntryPoint(void)
{
    DMSG("has been called");
    return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
    DMSG("has been called");
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types,
                    TEE_Param __unused params[4],
                    void __unused **sess_ctx)
{
    uint32_t exp_param_types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_NONE,
                           TEE_PARAM_TYPE_NONE,
                           TEE_PARAM_TYPE_NONE,
                           TEE_PARAM_TYPE_NONE);

    DMSG("has been called");

    if (param_types != exp_param_types)
        return TEE_ERROR_BAD_PARAMETERS;

    IMSG("PRISM TA session opened\n");
    return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void __unused *sess_ctx)
{
    IMSG("PRISM TA session closed\n");
}

/*
 * Verify + decrypt one AES-128-GCM buffer. On a bad tag (tampered ciphertext
 * or wrong key) TEE_AEDecryptFinal returns TEE_ERROR_MAC_INVALID and no
 * plaintext should be trusted.
 *
 * Note the tag-length unit mismatch in the GP API:
 *   - TEE_AEInit()        takes the tag length in BITS
 *   - TEE_AEDecryptFinal() takes the tag length in BYTES
 */
static TEE_Result decrypt_gcm(const uint8_t *iv,
                  const uint8_t *tag,
                  const uint8_t *ct, size_t ct_len,
                  uint8_t *pt, size_t *pt_len)
{
    TEE_OperationHandle op  = TEE_HANDLE_NULL;
    TEE_ObjectHandle    key = TEE_HANDLE_NULL;
    TEE_Attribute       attr;
    TEE_Result          res;

    res = TEE_AllocateOperation(&op, TEE_ALG_AES_GCM, TEE_MODE_DECRYPT,
                    AES_KEY_BITS);
    if (res != TEE_SUCCESS)
        return res;

    res = TEE_AllocateTransientObject(TEE_TYPE_AES, AES_KEY_BITS, &key);
    if (res != TEE_SUCCESS)
        goto out_op;

    TEE_InitRefAttribute(&attr, TEE_ATTR_SECRET_VALUE,
                 g_aes_key, sizeof(g_aes_key));

    res = TEE_PopulateTransientObject(key, &attr, 1);
    if (res != TEE_SUCCESS)
        goto out_key;

    res = TEE_SetOperationKey(op, key);
    if (res != TEE_SUCCESS)
        goto out_key;

    /* AAD/payload lengths are not needed up-front for GCM: pass 0. */
    res = TEE_AEInit(op, iv, GCM_IV_LEN, GCM_TAG_BITS, 0, 0);
    if (res != TEE_SUCCESS)
        goto out_key;

    res = TEE_AEDecryptFinal(op, ct, ct_len, pt, pt_len,
                 (void *)tag, GCM_TAG_LEN);

out_key:
    TEE_FreeTransientObject(key);
out_op:
    TEE_FreeOperation(op);
    return res;
}

/*
 * In-place RGB8 -> grayscale. Integer luma with weights that sum to 256:
 *   Y = (77*R + 150*G + 29*B) >> 8   (approx. 0.299/0.587/0.114)
 * Each pixel is written back as Y,Y,Y so the frame stays a valid 3-channel
 * rgb8 image of the same size. If the camera publishes bgr8, swap the 77 and
 * 29 weights so the luma is correct.
 */
static void grayscale_rgb(uint8_t *img, size_t len)
{
    for (size_t i = 0; i + 3 <= len; i += 3) {
        uint32_t r = img[i];
        uint32_t g = img[i + 1];
        uint32_t b = img[i + 2];
        uint8_t y = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
        img[i]     = y;
        img[i + 1] = y;
        img[i + 2] = y;
    }
}

/*
 * Shared buffer layout from the CA (must match EncryptedPublisher's framing):
 *   [ IV (12) ][ TAG (16) ][ CIPHERTEXT ]
 *
 * On success the decrypted, grayscaled frame is written back to the start of
 * the same buffer and params[0].memref.size is set to the plaintext length.
 */
static TEE_Result process_image(uint32_t param_types, TEE_Param params[4])
{
    uint32_t exp_param_types = TEE_PARAM_TYPES(
        TEE_PARAM_TYPE_MEMREF_INOUT,
        TEE_PARAM_TYPE_NONE,
        TEE_PARAM_TYPE_NONE,
        TEE_PARAM_TYPE_NONE);
    if (param_types != exp_param_types)
        return TEE_ERROR_BAD_PARAMETERS;

    uint8_t *buf = (uint8_t *)params[0].memref.buffer;
    uint32_t buf_size = params[0].memref.size;

    if (!buf || buf_size <= GCM_IV_LEN + GCM_TAG_LEN)
        return TEE_ERROR_BAD_PARAMETERS;

    const uint8_t *iv  = buf;
    const uint8_t *tag = buf + GCM_IV_LEN;
    const uint8_t *ct  = buf + GCM_IV_LEN + GCM_TAG_LEN;
    size_t ct_len = buf_size - GCM_IV_LEN - GCM_TAG_LEN;

    /* For GCM the plaintext is exactly as long as the ciphertext. Decrypt
     * into secure-world heap so the shared buffer is never a decrypt target
     * (avoids any src/dest overlap). */
    uint8_t *pt = TEE_Malloc(ct_len, TEE_MALLOC_FILL_ZERO);
    if (!pt)
        return TEE_ERROR_OUT_OF_MEMORY;

    size_t pt_len = ct_len;
    TEE_Result res = decrypt_gcm(iv, tag, ct, ct_len, pt, &pt_len);
    if (res != TEE_SUCCESS) {
        /* TEE_ERROR_MAC_INVALID => tampered frame or key mismatch. */
        EMSG("GCM decrypt/auth failed: 0x%08x", res);
        TEE_Free(pt);
        return res;
    }

    if (pt_len % 3 != 0) {
        EMSG("plaintext %zu not a multiple of 3 (expected rgb8)", pt_len);
        TEE_Free(pt);
        return TEE_ERROR_BAD_FORMAT;
    }

    grayscale_rgb(pt, pt_len);

    /* Hand the processed frame back at offset 0 and report its length.
     * buf_size >= pt_len (we dropped IV+TAG), so this always fits. */
    TEE_MemMove(buf, pt, pt_len);
    params[0].memref.size = pt_len;

    TEE_Free(pt);
    return TEE_SUCCESS;
}

TEE_Result TA_InvokeCommandEntryPoint(
    void __unused *sess_ctx,
    uint32_t cmd_id,
    uint32_t param_types,
    TEE_Param params[4])
{
    switch (cmd_id) {
        case TA_PROCESS_IMAGE_CMD:
            return process_image(param_types, params);
        default:
            return TEE_ERROR_BAD_PARAMETERS;
    }
}
