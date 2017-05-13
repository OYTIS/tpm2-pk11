/*
 * This file is part of tpm2-pk11.
 * Copyright (C) 2017 Iwan Timmer
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "pk11.h"

#include "config.h"
#include "sessions.h"
#include "utils.h"
#include "tpm.h"

#include <sys/mman.h>
#include <string.h>
#include <stdio.h>

#include <arpa/inet.h>
#include <p11-kit/pkcs11.h>

#define SLOT_ID 0x1234

#define get_session(x) ((struct session*) x)

static struct config pk11_config = {0};

CK_RV C_GetInfo(CK_INFO_PTR pInfo) {
  pInfo->cryptokiVersion.major = CRYPTOKI_VERSION_MAJOR;
  pInfo->cryptokiVersion.minor = CRYPTOKI_VERSION_MINOR;
  strncpy_pad(pInfo->manufacturerID, TPM2_PK11_MANUFACTURER, sizeof(pInfo->manufacturerID));
  strncpy_pad(pInfo->libraryDescription, TPM2_PK11_LIBRARY_DESCRIPTION, sizeof(pInfo->libraryDescription));
  pInfo->flags = 0;

  return CKR_OK;
}

CK_RV C_GetSlotList(CK_BBOOL tokenPresent, CK_SLOT_ID_PTR pSlotList, CK_ULONG_PTR pusCount) {
  if (*pusCount)
    *pSlotList = SLOT_ID;

  *pusCount = 1;

  return CKR_OK;
}

CK_RV C_OpenSession(CK_SLOT_ID slotID, CK_FLAGS flags, CK_VOID_PTR pApplication, CK_RV  (*Notify) (CK_SESSION_HANDLE hSession, CK_NOTIFICATION event, CK_VOID_PTR pApplication), CK_SESSION_HANDLE_PTR phSession) {
  *phSession = (unsigned long) malloc(sizeof(struct session));
  session_init((struct session*) *phSession, &pk11_config);

  return *phSession == 0 ? CKR_GENERAL_ERROR : CKR_OK;
}

CK_RV C_CloseSession(CK_SESSION_HANDLE hSession) {
  session_close(get_session(hSession));
  free(get_session(hSession));
  return CKR_OK;
}

CK_RV C_GetSessionInfo(CK_SESSION_HANDLE hSession, CK_SESSION_INFO_PTR pInfo) {
  pInfo->slotID = 0;
  pInfo->state = CKS_RO_USER_FUNCTIONS;
  pInfo->flags = CKF_SERIAL_SESSION;
  pInfo->ulDeviceError = 0;
  return CKR_OK;
}

CK_RV C_GetSlotInfo(CK_SLOT_ID slotID, CK_SLOT_INFO_PTR pInfo) {
  return CKR_OK;
}

CK_RV C_GetTokenInfo(CK_SLOT_ID slotID, CK_TOKEN_INFO_PTR pInfo) {
  strncpy_pad(pInfo->label, TPM2_PK11_LABEL, sizeof(pInfo->label));
  strncpy_pad(pInfo->manufacturerID, TPM2_PK11_MANUFACTURER, sizeof(pInfo->manufacturerID));
  strncpy_pad(pInfo->model, TPM2_PK11_MODEL, sizeof(pInfo->label));
  strncpy_pad(pInfo->serialNumber, TPM2_PK11_SERIAL, sizeof(pInfo->serialNumber));
  strncpy_pad(pInfo->utcTime, "", sizeof(pInfo->utcTime));

  pInfo->flags = CKF_TOKEN_INITIALIZED;
  pInfo->ulMaxSessionCount = 1;
  pInfo->ulSessionCount = 0;
  pInfo->ulMaxRwSessionCount = 1;
  pInfo->ulRwSessionCount = 0;
  pInfo->ulMaxPinLen = 64;
  pInfo->ulMinPinLen = 8;
  pInfo->ulTotalPublicMemory = 8;
  pInfo->ulFreePublicMemory = 8;
  pInfo->ulTotalPrivateMemory = 8;
  pInfo->ulFreePrivateMemory = 8;
  pInfo->hardwareVersion.major = 0;
  pInfo->firmwareVersion.major = 0;

  return CKR_OK;
}

CK_RV C_Finalize(CK_VOID_PTR pReserved) {
  return CKR_OK;
}

CK_RV C_FindObjectsInit(CK_SESSION_HANDLE hSession, CK_ATTRIBUTE_PTR filters, CK_ULONG nfilters) {
  get_session(hSession)->findPosition = 0;
  return CKR_OK;
}

CK_RV C_FindObjects(CK_SESSION_HANDLE hSession, CK_OBJECT_HANDLE_PTR phObject, CK_ULONG usMaxObjectCount, CK_ULONG_PTR nfound) {
  if (usMaxObjectCount == 0 || get_session(hSession)->findPosition >= 1)
    *nfound = 0;
  else {
    *nfound = 1;
    get_session(hSession)->findPosition++;
  }
  return CKR_OK;
}

CK_RV C_FindObjectsFinal(CK_SESSION_HANDLE hSession) {
  return CKR_OK;
}

CK_RV C_GetAttributeValue(CK_SESSION_HANDLE hSession, CK_OBJECT_HANDLE hObject, CK_ATTRIBUTE_PTR pTemplate, CK_ULONG usCount) {
  TPM2B_PUBLIC key = {0};
  TPM_RC ret = tpm_readpublic(get_session(hSession)->context, pk11_config.key_handle, &key);
  if (ret != TPM_RC_SUCCESS) {
    printf("%x\n", ret);
    return CKR_GENERAL_ERROR;
  }

  TPM2B_PUBLIC_KEY_RSA *rsa_key = &key.t.publicArea.unique.rsa;
  TPMS_RSA_PARMS *rsa_key_parms = &key.t.publicArea.parameters.rsaDetail;

  CK_OBJECT_CLASS object_class = CKO_PRIVATE_KEY;
  CK_BBOOL sign = CK_TRUE;
  CK_BBOOL decrypt = CK_TRUE;
  uint32_t exponent = htonl(rsa_key_parms->exponent == 0 ? 65537 : rsa_key_parms->exponent);

  for (int i = 0; i < usCount; i++) {
    switch (pTemplate[i].type) {
    case CKA_ID:
      pTemplate[i].ulValueLen = 8;
      break;
    case CKA_CLASS:
      retmem(pTemplate[i].pValue, &pTemplate[i].ulValueLen, &object_class, sizeof(CK_OBJECT_CLASS));
      break;
    case CKA_SIGN:
      retmem(pTemplate[i].pValue, &pTemplate[i].ulValueLen, &sign, sizeof(CK_BBOOL));
      break;
    case CKA_DECRYPT:
      retmem(pTemplate[i].pValue, &pTemplate[i].ulValueLen, &decrypt, sizeof(CK_BBOOL));
      break;
    case CKA_PUBLIC_EXPONENT:
      retmem(pTemplate[i].pValue, &pTemplate[i].ulValueLen, &exponent, sizeof(uint32_t));
      break;
    case CKA_MODULUS:
      retmem(pTemplate[i].pValue, &pTemplate[i].ulValueLen, rsa_key->b.buffer, rsa_key_parms->keyBits / 8);
      break;
    default:
      pTemplate[i].ulValueLen = 0;
    }
  }

  return CKR_OK;
}

CK_RV C_SignInit(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey) {
  return CKR_OK;
}

CK_RV C_Sign(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pData, CK_ULONG usDataLen, CK_BYTE_PTR pSignature, CK_ULONG_PTR pusSignatureLen) {
  TPMT_SIGNATURE signature = {0};
  TPM_RC ret = tpm_sign(get_session(hSession)->context, pk11_config.key_handle, pData, usDataLen, &signature);
  retmem(pSignature, pusSignatureLen, signature.signature.rsassa.sig.t.buffer, signature.signature.rsassa.sig.t.size);

  return ret == TPM_RC_SUCCESS ? CKR_OK : CKR_GENERAL_ERROR;
}

CK_RV C_DecryptInit(CK_SESSION_HANDLE hSession, CK_MECHANISM_PTR pMechanism, CK_OBJECT_HANDLE hKey) {
  return CKR_OK;
}

CK_RV C_Decrypt(CK_SESSION_HANDLE hSession, CK_BYTE_PTR pEncryptedData, CK_ULONG ulEncryptedDataLen, CK_BYTE_PTR pData, CK_ULONG_PTR pulDataLen) {
  TPM2B_PUBLIC_KEY_RSA message = { .t.size = MAX_RSA_KEY_BYTES };
  TPM_RC ret = tpm_decrypt(get_session(hSession)->context, pk11_config.key_handle, pEncryptedData, ulEncryptedDataLen, &message);
  retmem(pData, pulDataLen, message.t.buffer, message.t.size);

  return ret == TPM_RC_SUCCESS ? CKR_OK : CKR_GENERAL_ERROR;
}

CK_RV C_Initialize(CK_VOID_PTR pInitArgs) {
  char configfile_path[256];
  snprintf(configfile_path, sizeof(configfile_path), "%s/" TPM2_PK11_CONFIG_DIR "/" TPM2_PK11_CONFIG_FILE, getenv("HOME"));
  if (config_load(configfile_path, &pk11_config) < 0)
    return CKR_GENERAL_ERROR;

  return CKR_OK;
}

static CK_FUNCTION_LIST function_list = {
  { CRYPTOKI_VERSION_MAJOR, CRYPTOKI_VERSION_MINOR },
  .C_Initialize = C_Initialize,
  .C_Finalize = C_Finalize,
  .C_GetInfo = C_GetInfo,
  .C_GetSlotList = C_GetSlotList,
  .C_GetSlotInfo = C_GetSlotInfo,
  .C_GetTokenInfo = C_GetTokenInfo,
  .C_OpenSession = C_OpenSession,
  .C_CloseSession = C_CloseSession,
  .C_GetSessionInfo = C_GetSessionInfo,
  .C_GetAttributeValue = C_GetAttributeValue,
  .C_FindObjectsInit = C_FindObjectsInit,
  .C_FindObjects = C_FindObjects,
  .C_FindObjectsFinal = C_FindObjectsFinal,
  .C_SignInit = C_SignInit,
  .C_Sign = C_Sign,
  .C_DecryptInit = C_DecryptInit,
  .C_Decrypt = C_Decrypt
};

CK_RV C_GetFunctionList(CK_FUNCTION_LIST_PTR_PTR ppFunctionList) {
  if (ppFunctionList == NULL_PTR)
    return CKR_ARGUMENTS_BAD;

  *ppFunctionList = &function_list;
  return CKR_OK;
}
